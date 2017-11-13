////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2017 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBSphericalIndex.h"

#include "Aql/Ast.h"
#include "Aql/AstNode.h"
#include "Aql/SortCondition.h"
#include "Basics/StringRef.h"
#include "Basics/VelocyPackHelper.h"
#include "Geo/GeoCover.h"
#include "Geo/GeoJsonParser.h"
#include "Indexes/IndexResult.h"
#include "Logger/Logger.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBMethods.h"

#include <geometry/s2regioncoverer.h>
#include <rocksdb/db.h>

using namespace arangodb;

RocksDBSphericalIndexIterator::RocksDBSphericalIndexIterator(
    LogicalCollection* collection, transaction::Methods* trx,
    ManagedDocumentResult* mmdr, RocksDBSphericalIndex const* index)
    : IndexIterator(collection, trx, mmdr, index), _index(index) {
  RocksDBMethods* mthds = RocksDBTransactionState::toMethods(trx);
  rocksdb::ReadOptions options = mthds->readOptions();
  TRI_ASSERT(options.prefix_same_as_start);
  _iterator = mthds->NewIterator(options, _index->columnFamily());
  TRI_ASSERT(_index->columnFamily()->GetID() ==
             RocksDBColumnFamily::geo()->GetID());
}

class RegionIterator : public RocksDBSphericalIndexIterator {
 public:
  RegionIterator(LogicalCollection* collection, transaction::Methods* trx,
                 ManagedDocumentResult* mmdr,
                 RocksDBSphericalIndex const* index,
                 std::unique_ptr<S2Region> region, geo::FilterType ft)
      : RocksDBSphericalIndexIterator(collection, trx, mmdr, index),
        _region(region.release()),
        _filterType(ft) {}

  ~RegionIterator() { delete _region; }

  geo::FilterType filterType() const override { return _filterType; };

  bool nextDocument(DocumentCallback const& cb, size_t limit) override {}

  /// if you call this it's your own damn fault
  bool next(LocalDocumentIdCallback const& cb, size_t limit) override {}

  void reset() override {
    _seen.clear();
    // TODO
  }

 private:
  S2Region* const _region;
  geo::FilterType const _filterType;
  std::unordered_set<TRI_voc_rid_t> _seen;
  std::vector<geo::GeoCover::Interval> _intervals;
};

class NearIterator final : public RocksDBSphericalIndexIterator {
 public:
  /// @brief Construct an RocksDBGeoIndexIterator based on Ast Conditions
  NearIterator(LogicalCollection* collection, transaction::Methods* trx,
               ManagedDocumentResult* mmdr, RocksDBSphericalIndex const* index,
               geo::NearParams const& params)
      : RocksDBSphericalIndexIterator(collection, trx, mmdr, index),
        _near(params) {
    estimateDensity();
  }

  geo::FilterType filterType() const override { return _near.filterType(); }

  bool next(LocalDocumentIdCallback const& cb, size_t limit) override {
    if (_near.isDone()) {
      // we already know that no further results will be returned by the index
      TRI_ASSERT(!_near.hasNearest());
      return false;
    }

    while (limit > 0 && !_near.isDone()) {
      while (limit > 0 && _near.hasNearest()) {
        cb(LocalDocumentId(_near.nearest().rid));
        _near.popNearest();
        limit--;
      }
      // need to fetch more geo results
      if (limit > 0 && !_near.isDone()) {
        TRI_ASSERT(!_near.hasNearest());
        performScan();
      }
    }

    return !_near.isDone();
  }

  void reset() override { _near.reset(); }

 private:
  // we need to get intervals representing areas in a ring (annulus)
  // around our target point. We need to fetch them ALL and then sort
  // found results in a priority list according to their distance
  void performScan() {
    // list of sorted intervals to scan
    std::vector<geo::GeoCover::Interval> const scan = _near.intervals();
    LOG_TOPIC(INFO, Logger::FIXME) << "# scans: " << scan.size();

    for (size_t i = 0; i < scan.size(); i++) {
      geo::GeoCover::Interval const& it = scan[i];
      TRI_ASSERT(it.min <= it.max);

      // LOG_TOPIC(INFO, Logger::FIXME) << "[Seeking] " << it.min << " - " <<
      bool seek = true;            // we might have performed a seek already
      if (i > 0 && _iterator->Valid()) {  // we're somewhere in the index
        // don't bother to seek if we're already past the interval
        TRI_ASSERT(scan[i - 1].max < it.min);
        uint64_t cellId = RocksDBKey::sphericalValue(_iterator->key());
        if (it.max.id() < cellId) {
          continue;  // out of range
        } else if (it.min.id() < cellId) {
          seek = false;  // already in range!!
        }                // TODO next() instead of seek sometimes?
      }

      // try to avoid seeking at all cost
      if (seek) {
        RocksDBKeyBounds bounds = RocksDBKeyBounds::SphericalIndex(
            _index->objectId(), it.min.id(), it.max.id());
        _iterator->Seek(bounds.start());
      }

      while (_iterator->Valid()) {
        uint64_t cellId = RocksDBKey::sphericalValue(_iterator->key());
        TRI_ASSERT(it.min.id() <= cellId);
        if (it.max.id() < cellId) {
          break;  // out of range
        }

        TRI_voc_rid_t rid = RocksDBKey::revisionId(
            RocksDBEntryType::SphericalIndexValue, _iterator->key());
        geo::Coordinate cntrd = RocksDBValue::centroid(_iterator->value());
        _near.reportFound(rid, cntrd);

        // LOG_TOPIC(ERR, Logger::FIXME) << "[Found] " << S2CellId(cellId)
        //  << " | rid: " << rid << " at " << cntrd.latitude << ", " <<
        //  cntrd.longitude;
        _iterator->Next();
      }
    }
  }

  /// find the first indexed entry to estimate the # of entries
  /// around our target coordinates
  void estimateDensity() {
    S2CellId cell = S2CellId::FromPoint(_near.centroid());

    RocksDBKeyLeaser key(_trx);
    key->constructSphericalIndexValue(_index->objectId(), cell.id(), 0);
    _iterator->Seek(key->string());
    if (!_iterator->Valid()) {
      _iterator->SeekForPrev(key->string());
    }
    if (_iterator->Valid()) {
      geo::Coordinate first = RocksDBValue::centroid(_iterator->value());
      _near.estimateDensity(first);
    } else {
      LOG_TOPIC(INFO, Logger::ROCKSDB)
          << "Apparently the spherical index is empty";
      _near.invalidate();
    }
  }

 private:
  geo::NearUtils _near;
};

RocksDBSphericalIndex::RocksDBSphericalIndex(TRI_idx_iid_t iid,
                                             LogicalCollection* collection,
                                             VPackSlice const& info)
    : RocksDBIndex(iid, collection, info, RocksDBColumnFamily::geo(), false),
      _variant(IndexVariant::NONE) {
  TRI_ASSERT(iid != 0);
  _unique = false;
  _sparse = true;

  _coverParams.fromVelocyPack(info);
  if (_fields.size() == 1) {
    bool geoJson =
        basics::VelocyPackHelper::getBooleanValue(info, "geoJson", false);
    // geojson means [<longitude>, <latitude>] or
    // json object {type:"<name>, coordinates:[]}.
    _variant = geoJson ? IndexVariant::COMBINED_GEOJSON
                       : IndexVariant::COMBINED_LAT_LON;

    auto& loc = _fields[0];
    _location.reserve(loc.size());
    for (auto const& it : loc) {
      _location.emplace_back(it.name);
    }
  } else if (_fields.size() == 2) {
    _variant = IndexVariant::INDIVIDUAL_LAT_LON;
    auto& lat = _fields[0];
    _latitude.reserve(lat.size());
    for (auto const& it : lat) {
      _latitude.emplace_back(it.name);
    }
    auto& lon = _fields[1];
    _longitude.reserve(lon.size());
    for (auto const& it : lon) {
      _longitude.emplace_back(it.name);
    }
  } else {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "RocksDBGeoIndex can only be created with one or two fields.");
  }
}

/// @brief return a JSON representation of the index
void RocksDBSphericalIndex::toVelocyPack(VPackBuilder& builder,
                                         bool withFigures,
                                         bool forPersistence) const {
  builder.openObject();
  // Basic index
  RocksDBIndex::toVelocyPack(builder, withFigures, forPersistence);

  builder.add("geoJson",
              VPackValue(_variant == IndexVariant::COMBINED_GEOJSON));
  // geo indexes are always non-unique
  // geo indexes are always sparse.
  // "ignoreNull" has the same meaning as "sparse" and is only returned for
  // backwards compatibility
  // the "constraint" attribute has no meaning since ArangoDB 2.5 and is only
  // returned for backwards compatibility
  builder.add("constraint", VPackValue(false));
  builder.add("unique", VPackValue(false));
  builder.add("ignoreNull", VPackValue(true));
  builder.add("sparse", VPackValue(true));
  builder.close();
}

/// @brief Test if this index matches the definition
bool RocksDBSphericalIndex::matchesDefinition(VPackSlice const& info) const {
  TRI_ASSERT(info.isObject());
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  VPackSlice typeSlice = info.get("type");
  TRI_ASSERT(typeSlice.isString());
  StringRef typeStr(typeSlice);
  TRI_ASSERT(typeStr == oldtypeName());
#endif
  auto value = info.get("id");
  if (!value.isNone()) {
    // We already have an id.
    if (!value.isString()) {
      // Invalid ID
      return false;
    }
    // Short circuit. If id is correct the index is identical.
    StringRef idRef(value);
    return idRef == std::to_string(_iid);
  }

  if (_unique !=
      basics::VelocyPackHelper::getBooleanValue(info, "unique", false)) {
    return false;
  }
  if (_sparse !=
      basics::VelocyPackHelper::getBooleanValue(info, "sparse", true)) {
    return false;
  }

  value = info.get("fields");
  if (!value.isArray()) {
    return false;
  }

  size_t const n = static_cast<size_t>(value.length());
  if (n != _fields.size()) {
    return false;
  }

  if (n == 1) {
    bool geoJson =
        basics::VelocyPackHelper::getBooleanValue(info, "geoJson", false);
    if (geoJson && _variant != IndexVariant::COMBINED_GEOJSON) {
      return false;
    }
  }

  // This check takes ordering of attributes into account.
  std::vector<arangodb::basics::AttributeName> translate;
  for (size_t i = 0; i < n; ++i) {
    translate.clear();
    VPackSlice f = value.at(i);
    if (!f.isString()) {
      // Invalid field definition!
      return false;
    }
    arangodb::StringRef in(f);
    TRI_ParseAttributeString(in, translate, true);
    if (!arangodb::basics::AttributeName::isIdentical(_fields[i], translate,
                                                      false)) {
      return false;
    }
  }
  return true;
}

/// @brief creates an IndexIterator for the given Condition
IndexIterator* RocksDBSphericalIndex::iteratorForCondition(
    transaction::Methods* trx, ManagedDocumentResult* mmdr,
    arangodb::aql::AstNode const* node,
    arangodb::aql::Variable const* reference, bool reverse) {
  TRI_ASSERT(node != nullptr);

  size_t numMembers = node->numMembers();

  TRI_ASSERT(numMembers == 1);  // should only be an FCALL
  auto fcall = node->getMember(0);
  TRI_ASSERT(fcall->type == arangodb::aql::NODE_TYPE_FCALL);
  TRI_ASSERT(fcall->numMembers() == 1);
  auto args = fcall->getMember(0);

  numMembers = args->numMembers();
  TRI_ASSERT(numMembers >= 3);

  geo::Coordinate center(/*lat*/ args->getMember(1)->getDoubleValue(),
                         /*lon*/ args->getMember(2)->getDoubleValue());
  geo::NearParams params(center);
  if (numMembers == 5) {
    // WITHIN
    params.maxDistance = args->getMember(3)->getDoubleValue();
    params.maxInclusive = args->getMember(4)->getBoolValue();
  }

  // params.cover.worstIndexedLevel < _coverParams.worstIndexedLevel
  // is not necessary, > would be missing entries.
  params.cover.worstIndexedLevel = _coverParams.worstIndexedLevel;
  if (params.cover.bestIndexedLevel > _coverParams.bestIndexedLevel) {
    // it is unnessesary to have the level smaller
    params.cover.bestIndexedLevel = _coverParams.bestIndexedLevel;
  }

  return new NearIterator(_collection, trx, mmdr, this, params);
}

Result RocksDBSphericalIndex::parse(VPackSlice const& doc,
                                    std::vector<S2CellId>& cells,
                                    geo::Coordinate& centroid) const {
  if (_variant == IndexVariant::COMBINED_GEOJSON) {
    S2RegionCoverer coverer;
    _coverParams.configureS2RegionCoverer(&coverer);
    VPackSlice loc = doc.get(_location);
    return geo::GeoCover::generateCoverJson(&coverer, loc, cells, centroid);
  } else if (_variant == IndexVariant::COMBINED_LAT_LON) {
    VPackSlice loc = doc.get(_location);
    return geo::GeoCover::generateCoverLatLng(loc, false, cells, centroid);
  } else if (_variant == IndexVariant::INDIVIDUAL_LAT_LON) {
    VPackSlice lon = doc.get(_longitude);
    VPackSlice lat = doc.get(_latitude);
    if (!lon.isNumber() || !lat.isNumber()) {
      return TRI_ERROR_BAD_PARAMETER;
    }
    centroid.latitude = lat.getNumericValue<double>();
    centroid.longitude = lon.getNumericValue<double>();

    return geo::GeoCover::generateCover(centroid, cells);
  }
  return TRI_ERROR_INTERNAL;
}

/// internal insert function, set batch or trx before calling
Result RocksDBSphericalIndex::insertInternal(transaction::Methods* trx,
                                             RocksDBMethods* mthd,
                                             LocalDocumentId const& documentId,
                                             velocypack::Slice const& doc) {
  // covering and centroid of coordinate / polygon / ...
  std::vector<S2CellId> cells;
  geo::Coordinate centroid(-1, -1);

  Result res = parse(doc, cells, centroid);
  if (res.fail()) {
    // Invalid, no insert. Index is sparse
    return res.is(TRI_ERROR_BAD_PARAMETER) ? IndexResult() : res;
  }

  RocksDBValue val = RocksDBValue::SphericalValue(centroid);
  RocksDBKeyLeaser key(trx);

  // FIXME: can we rely on the region coverer to return
  // the same cells everytime for the same parameters ?
  for (S2CellId cell : cells) {
    // LOG_TOPIC(INFO, Logger::FIXME) << "[Insert] " << cell;

    key->constructSphericalIndexValue(_objectId, cell.id(), documentId.id());
    Result r = mthd->Put(RocksDBColumnFamily::geo(), key.ref(), val.string());
    if (r.fail()) {
      return r;
    }
  }

  return IndexResult();
}

/// internal remove function, set batch or trx before calling
Result RocksDBSphericalIndex::removeInternal(transaction::Methods* trx,
                                             RocksDBMethods* mthd,
                                             LocalDocumentId const& documentId,
                                             VPackSlice const& doc) {
  // covering and centroid of coordinate / polygon / ...
  std::vector<S2CellId> cells;
  geo::Coordinate centroid(-1, -1);

  Result res = parse(doc, cells, centroid);
  if (res.fail()) {
    // Invalid, no insert. Index is sparse
    return res.is(TRI_ERROR_BAD_PARAMETER) ? IndexResult() : res;
  }

  RocksDBKeyLeaser key(trx);
  // FIXME: can we rely on the region coverer to return
  // the same cells everytime for the same parameters ?
  for (S2CellId cell : cells) {
    // LOG_TOPIC(INFO, Logger::FIXME) << "[Remove] " << cell;

    key->constructSphericalIndexValue(_objectId, cell.id(), documentId.id());
    Result r = mthd->Delete(RocksDBColumnFamily::geo(), key.ref());
    if (r.fail()) {
      return r;
    }
  }

  return IndexResult();
}

void RocksDBSphericalIndex::truncate(transaction::Methods* trx) {
  RocksDBIndex::truncate(trx);
  // GeoIndex_reset(_geoIndex, RocksDBTransactionState::toMethods(trx));
}
