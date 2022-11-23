/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SHARE

#include "share/ob_tablet_checksum_operator.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/tablet/ob_tablet_to_ls_operator.h"
#include "lib/mysqlclient/ob_isql_client.h"
#include "lib/mysqlclient/ob_mysql_result.h"
#include "lib/mysqlclient/ob_mysql_proxy.h"
#include "lib/string/ob_sql_string.h"
#include "common/ob_smart_var.h"
#include "lib/mysqlclient/ob_mysql_transaction.h"

namespace oceanbase
{
namespace share
{
using namespace oceanbase::common;
using namespace oceanbase::common::sqlclient;
using namespace oceanbase::transaction;

void ObTabletChecksumItem::reset()
{
  tenant_id_ = OB_INVALID_TENANT_ID;
  tablet_id_.reset();
  ls_id_.reset();
  data_checksum_ = -1;
  row_count_ = 0;
  snapshot_version_ = 0;
  column_meta_.reset();
}

bool ObTabletChecksumItem::is_valid() const
{
  return (OB_INVALID_TENANT_ID != tenant_id_) && (tablet_id_.is_valid()) && (ls_id_.is_valid());
}

ObTabletChecksumItem &ObTabletChecksumItem::operator=(const ObTabletChecksumItem &other)
{
  tenant_id_ = other.tenant_id_;
  tablet_id_ = other.tablet_id_;
  ls_id_ = other.ls_id_;
  data_checksum_ = other.data_checksum_;
  row_count_ = other.row_count_;
  snapshot_version_ = other.snapshot_version_;
  column_meta_.assign(other.column_meta_);
  return *this;
}

bool ObTabletChecksumItem::is_same_tablet(const ObTabletChecksumItem &item) const
{
  return (tenant_id_ == item.tenant_id_) && (tablet_id_ == item.tablet_id_) 
    && (ls_id_ == item.ls_id_);
}

int ObTabletChecksumItem::compare_tablet(const ObTabletReplicaChecksumItem &replica_item) const
{
  int ret = 0;
  if (tenant_id_ == replica_item.tenant_id_) {
    if (tablet_id_.id() < replica_item.tablet_id_.id()) {
      ret = -1;
    } else if (tablet_id_.id() > replica_item.tablet_id_.id()) {
      ret = 1;
    } else {
      if (ls_id_.id() < replica_item.ls_id_.id()) {
        ret = -1;
      } else if (ls_id_.id() > replica_item.ls_id_.id()) {
        ret = 1;
      }
    }
  }
  return ret;
}

int ObTabletChecksumItem::verify_tablet_checksum(const ObTabletReplicaChecksumItem &replica_item) const
{
  int ret = OB_SUCCESS;
  if ((tenant_id_ != replica_item.tenant_id_)
      || (tablet_id_ != replica_item.tablet_id_)
      || (ls_id_ != replica_item.ls_id_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(replica_item), K(*this));
  } else {
    // cmp data_checksum/row_count/column_checksum in same snapshot_version
    if (snapshot_version_ == replica_item.snapshot_version_) {
      bool is_same_column_checksum = false;
      if (OB_FAIL(column_meta_.check_equal(replica_item.column_meta_, is_same_column_checksum))) {
        LOG_WARN("fail to check column_meta equal", KR(ret), K(replica_item));
      } else if ((data_checksum_ != replica_item.data_checksum_)
                 || (row_count_ != replica_item.row_count_)
                 || !is_same_column_checksum) {
        ret = OB_CHECKSUM_ERROR;
        LOG_ERROR("fatal checksum error", KR(ret), K(is_same_column_checksum), K(replica_item), K(*this));
      }
    } 
  }
  return ret;
}

int ObTabletChecksumItem::assign(const ObTabletReplicaChecksumItem &replica_item)
{
  int ret = OB_SUCCESS;
  if (!replica_item.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(replica_item));
  } else {
    tenant_id_ = replica_item.tenant_id_;
    tablet_id_ = replica_item.tablet_id_;
    ls_id_ = replica_item.ls_id_;
    data_checksum_ = replica_item.data_checksum_;
    row_count_ = replica_item.row_count_;
    snapshot_version_ = replica_item.snapshot_version_;
    if (OB_FAIL(column_meta_.assign(replica_item.column_meta_))) {
      LOG_WARN("fail to assign column meta", KR(ret), K(replica_item));
    }
  }
  return ret;
}

///////////////////////////////////////////////////////////////////////////////

int ObTabletChecksumOperator::load_tablet_checksum_items(
    ObISQLClient &sql_client,
    const ObTabletLSPair &start_pair,
    const int64_t batch_cnt,
    const uint64_t tenant_id,
    const int64_t snapshot_version,
    ObIArray<ObTabletChecksumItem> &items)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id)
      || (snapshot_version <= OB_INVALID_VERSION)
      || (batch_cnt < 1))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", KR(ret), K(tenant_id), K(batch_cnt), K(snapshot_version));
  } 
  int64_t remain_cnt = batch_cnt;
  int64_t ori_items_cnt = 0;
  ObSqlString sql;
  while (OB_SUCC(ret) && (remain_cnt > 0)) {
    sql.reuse();
    const int64_t limit_cnt = ((remain_cnt >= MAX_BATCH_COUNT) ? MAX_BATCH_COUNT : remain_cnt);
    ori_items_cnt = items.count();

    ObTabletLSPair last_pair;
    if (remain_cnt == batch_cnt) {
      last_pair = start_pair;
    } else {
      const int64_t items_cnt = items.count();
      if (items_cnt > 0) {
        const ObTabletChecksumItem &last_item = items.at(items_cnt - 1);
        if (OB_FAIL(last_pair.init(last_item.tablet_id_, last_item.ls_id_))) {
          LOG_WARN("fail to init last tablet_ls_pair", KR(ret), K(last_item));
        }
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("err unexpected, about tablet items count", KR(ret), K(tenant_id), K(start_pair),
          K(batch_cnt), K(remain_cnt), K(items_cnt));
      }
    }

    if (FAILEDx(construct_load_sql_str_(tenant_id, last_pair, limit_cnt, snapshot_version, sql))) {
      LOG_WARN("fail to construct load sql", KR(ret), K(tenant_id), K(last_pair), K(limit_cnt), 
        K(snapshot_version));
    } else if (OB_FAIL(load_tablet_checksum_items(sql_client, sql, tenant_id, items))) {
      LOG_WARN("fail to load tablet checksum items", KR(ret), K(tenant_id), K(sql));
    } else {
      const int64_t curr_items_cnt = items.count();
      if (curr_items_cnt - ori_items_cnt == limit_cnt) {
        remain_cnt -= limit_cnt;
      } else {
        remain_cnt = 0;
      }
    }
  }
  return ret;
}

int ObTabletChecksumOperator::load_tablet_checksum_items(
    ObISQLClient &sql_client,
    const ObIArray<ObTabletLSPair> &pairs,
    const uint64_t tenant_id,
    ObIArray<ObTabletChecksumItem> &items)
{
  int ret = OB_SUCCESS;
  const int64_t pairs_cnt = pairs.count();
  int64_t start_idx = 0;
  int64_t end_idx = min(MAX_BATCH_COUNT, pairs_cnt);
  ObSqlString sql;
  while (OB_SUCC(ret) && (start_idx < end_idx)) {
    sql.reuse();
    if (OB_FAIL(construct_load_sql_str_(tenant_id, pairs, start_idx, end_idx, sql))) {
      LOG_WARN("fail to construct load sql", KR(ret), K(tenant_id), K(pairs_cnt), 
        K(start_idx), K(end_idx));
    } else if (OB_FAIL(load_tablet_checksum_items(sql_client, sql, tenant_id, items))) {
      LOG_WARN("fail to load tablet checksum items", KR(ret), K(tenant_id), K(sql));
    } else {
      start_idx = end_idx;
      end_idx = min(start_idx + MAX_BATCH_COUNT, pairs_cnt);
    }
  }
  return ret;
}

int ObTabletChecksumOperator::load_tablet_checksum_items(
    ObISQLClient &sql_client,
    const ObSqlString &sql,
    const uint64_t tenant_id,
    ObIArray<ObTabletChecksumItem> &items)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id));
  } else {
    SMART_VAR(ObISQLClient::ReadResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_UNLIKELY(!sql.is_valid())) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid arguments", KR(ret), K(sql), K(tenant_id));
      } else if (OB_FAIL(sql_client.read(res, tenant_id, sql.ptr()))) {
        LOG_WARN("fail to execute sql", KR(ret), K(sql), K(tenant_id));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("error unexpected, query result must not be NULL", KR(ret), K(sql), K(tenant_id));
      } else {
        while (OB_SUCC(ret)) {
          ObTabletChecksumItem item;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END != ret) {
              LOG_WARN("fail to get next row", KR(ret), K(tenant_id));
            }
          } else {
            int64_t ls_id = ObLSID::INVALID_LS_ID;
            int64_t tenant_id = -1;
            int64_t tablet_id = -1;
            ObString column_meta_str;
            EXTRACT_INT_FIELD_MYSQL(*result, "tenant_id", tenant_id, int64_t);
            EXTRACT_UINT_FIELD_MYSQL(*result, "compaction_scn", item.snapshot_version_, uint64_t);
            EXTRACT_INT_FIELD_MYSQL(*result, "tablet_id", tablet_id, int64_t);
            EXTRACT_INT_FIELD_MYSQL(*result, "ls_id", ls_id, int64_t);
            EXTRACT_INT_FIELD_MYSQL(*result, "data_checksum", item.data_checksum_, int64_t);
            EXTRACT_INT_FIELD_MYSQL(*result, "row_count", item.row_count_, int64_t);
            EXTRACT_VARCHAR_FIELD_MYSQL(*result, "column_checksums", column_meta_str);
            if (OB_SUCC(ret)) {
              item.tenant_id_ = (uint64_t)tenant_id;
              item.tablet_id_ = (uint64_t)tablet_id;
              item.ls_id_ = ls_id;
              if (OB_FAIL(ObTabletReplicaChecksumOperator::set_column_meta_with_hex_str(column_meta_str, item.column_meta_))) {
                LOG_WARN("fail to deserialize column meta from hex str", KR(ret));
              } else if (OB_FAIL(items.push_back(item))) {
                LOG_WARN("fail to push back item", KR(ret), K(item));
              }
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int ObTabletChecksumOperator::construct_load_sql_str_(
    const uint64_t tenant_id,
    const ObTabletLSPair &start_pair,
    const int64_t batch_cnt,
    const int64_t snapshot_version,
    ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if ((batch_cnt < 1) || (snapshot_version <= OB_INVALID_VERSION)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(batch_cnt), K(snapshot_version));
  } else {
    if (snapshot_version > 0) {
      if (OB_FAIL(sql.append_fmt("SELECT * FROM %s WHERE tenant_id = '%lu' and compaction_scn = %lu "
          "and tablet_id >= '%lu' and ls_id > %ld ", OB_ALL_TABLET_CHECKSUM_TNAME, tenant_id,
          (uint64_t)(snapshot_version), start_pair.get_tablet_id().id(), start_pair.get_ls_id().id()))) {
        LOG_WARN("fail to assign sql", KR(ret), K(tenant_id), K(start_pair), K(snapshot_version));
      } else if (OB_FAIL(sql.append_fmt(" ORDER BY tenant_id, tablet_id, ls_id limit %ld", batch_cnt))) {
        LOG_WARN("fail to assign sql string", KR(ret), K(tenant_id), K(batch_cnt), K(snapshot_version));
      }
    } else { // snapshot_version == 0: get records with all snapshot_version
      if (OB_FAIL(sql.append_fmt("SELECT * FROM %s WHERE tenant_id = '%lu' and tablet_id >= '%lu' "
          "and ls_id > %ld ", OB_ALL_TABLET_CHECKSUM_TNAME, tenant_id, start_pair.get_tablet_id().id(),
          start_pair.get_ls_id().id()))) {
        LOG_WARN("fail to assign sql", KR(ret), K(tenant_id), K(start_pair), K(snapshot_version));
      } else if (OB_FAIL(sql.append_fmt(" ORDER BY tenant_id, tablet_id, ls_id, compaction_scn limit %ld",
          batch_cnt))) {
        LOG_WARN("fail to assign sql string", KR(ret), K(tenant_id), K(batch_cnt), K(snapshot_version));
      }
    }
  }
  return ret;
}

int ObTabletChecksumOperator::construct_load_sql_str_(
    const uint64_t tenant_id,
    const common::ObIArray<ObTabletLSPair> &pairs,
    const int64_t start_idx,
    const int64_t end_idx,
    common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const int64_t pairs_cnt = pairs.count();
  if ((start_idx < 0) || (end_idx > pairs_cnt) || 
      (start_idx > end_idx) || (pairs_cnt < 1)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(start_idx), K(end_idx), K(pairs_cnt));
  } else if (OB_FAIL(sql.append_fmt("SELECT * FROM %s WHERE tenant_id = '%lu' and (tablet_id, ls_id) "
      "IN (", OB_ALL_TABLET_CHECKSUM_TNAME, tenant_id))) {
    LOG_WARN("fail to assign sql", KR(ret), K(tenant_id));
  } else {
    for (int64_t idx = start_idx; OB_SUCC(ret) && (idx < end_idx); ++idx) {
      const ObTabletLSPair &pair = pairs.at(idx);
      if (OB_UNLIKELY(!pair.is_valid())) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid tablet_ls_pair", KR(ret), K(tenant_id), K(pair), K(idx));
      } else if (OB_FAIL(sql.append_fmt(
          "(%ld,%ld)%s",
          pair.get_tablet_id().id(),
          pair.get_ls_id().id(),
          ((idx == end_idx - 1) ? ")" : ", ")))) {
        LOG_WARN("fail to assign sql", KR(ret), K(tenant_id), K(pair));
      }
    }
  }

  if (FAILEDx(sql.append_fmt(") ORDER BY tenant_id, tablet_id, ls_id, compaction_scn"))) {
    LOG_WARN("fail to assign sql string", KR(ret), K(tenant_id), K(pairs_cnt));
  }
  return ret;
}

int ObTabletChecksumOperator::insert_tablet_checksum_item(
    ObISQLClient &sql_client,
    const uint64_t tenant_id,
    const ObTabletChecksumItem &item)
{
  int ret = OB_SUCCESS;
  ObArray<ObTabletChecksumItem> items;
  if (OB_FAIL(items.push_back(item))) {
    LOG_WARN("fail to add item into array", KR(ret), K(item));
  } else if (OB_FAIL(insert_tablet_checksum_items(sql_client, tenant_id, items))) {
    LOG_WARN("fail to insert tablet checksum items", KR(ret), K(item));
  }
  return ret;
}

int ObTabletChecksumOperator::insert_tablet_checksum_items(
    ObISQLClient &sql_client,
    const uint64_t tenant_id,
    ObIArray<ObTabletChecksumItem> &items)
{
  return insert_or_update_tablet_checksum_items_(sql_client, tenant_id, items, false/*is_update*/);
}

int ObTabletChecksumOperator::update_tablet_checksum_items(
    ObISQLClient &sql_client,
    const uint64_t tenant_id,
    ObIArray<ObTabletChecksumItem> &items)
{
  return insert_or_update_tablet_checksum_items_(sql_client, tenant_id, items, true/*is_update*/);
}

int ObTabletChecksumOperator::insert_or_update_tablet_checksum_items_(
    ObISQLClient &sql_client,
    const uint64_t tenant_id,
    ObIArray<ObTabletChecksumItem> &items,
    const bool is_update)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObMySQLTransaction trans;
  const int64_t item_cnt = items.count();
  if (OB_UNLIKELY(item_cnt < 1)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", KR(ret), K(item_cnt), K(tenant_id));
  } else if (OB_FAIL(trans.start(&sql_client, tenant_id))) {
    LOG_WARN("failed to start transaction", KR(ret), K(tenant_id));
  } else {
    int64_t remain_cnt = item_cnt; 
    int64_t report_idx = 0;
    while (OB_SUCC(ret) && (remain_cnt > 0)) {
      sql.reuse();
      if (OB_FAIL(sql.assign_fmt("INSERT INTO %s (tenant_id, compaction_scn, tablet_id, ls_id, data_checksum, "
          "row_count, column_checksums, gmt_modified, gmt_create) VALUES", OB_ALL_TABLET_CHECKSUM_TNAME))) {
        LOG_WARN("fail to assign sql", KR(ret), K(tenant_id));
      } else {
        ObArenaAllocator allocator;

        int64_t cur_batch_cnt = ((remain_cnt < MAX_BATCH_COUNT) ? remain_cnt : MAX_BATCH_COUNT);
        int64_t bias = item_cnt - remain_cnt;
        for (int64_t i = 0; OB_SUCC(ret) && (i < cur_batch_cnt); ++i) {
          const ObTabletChecksumItem &item = items.at(bias + i);
          const uint64_t compaction_scn = item.snapshot_version_ < 0 ? 0 : item.snapshot_version_;
          ObString hex_column_meta;

          if (OB_FAIL(ObTabletReplicaChecksumOperator::get_hex_column_meta(
              item.column_meta_, allocator, hex_column_meta))) {
            LOG_WARN("fail to serialize column meta to hex str", KR(ret), K(item));
          } else if (OB_FAIL(sql.append_fmt("('%lu', %lu, '%lu', %ld, %ld, %ld, '%.*s', now(6), now(6))%s",
              item.tenant_id_, compaction_scn, item.tablet_id_.id(), item.ls_id_.id(),
              item.data_checksum_, item.row_count_, hex_column_meta.length(), hex_column_meta.ptr(),
              ((i == cur_batch_cnt - 1) ? " " : ", ")))) {
            LOG_WARN("fail to assign sql", KR(ret), K(i), K(bias), K(item));
          }
        }

        if (OB_SUCC(ret) && is_update) {
          if (OB_FAIL(sql.append(" ON DUPLICATE KEY UPDATE "))) {
            LOG_WARN("fail to append sql string", KR(ret), K(sql));
          } else if (OB_FAIL(sql.append(" data_checksum = values(data_checksum)"))
                    || OB_FAIL(sql.append(", row_count = values(row_count)"))
                    || OB_FAIL(sql.append(", column_checksums = values(column_checksums)"))) {
            LOG_WARN("fail to append sql string", KR(ret), K(sql));
          }
        }

        if (OB_SUCC(ret)) {
          int64_t affected_rows = 0;
          if (OB_FAIL(sql_client.write(sql.ptr(), affected_rows))) {
            LOG_WARN("fail to execute sql", KR(ret), K(sql));
          } else if (OB_UNLIKELY(affected_rows > cur_batch_cnt)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("invalid affected rows", KR(ret), K(tenant_id), K(affected_rows), K(cur_batch_cnt));
          } else {
            remain_cnt -= cur_batch_cnt;
          }
        }
      }
    } // end loop while

    if (OB_SUCC(ret)) {
      if (OB_FAIL(trans.end(true /*commit*/))) {
        LOG_WARN("failed to commit trans", KR(ret), K(tenant_id));
      }
    } else {
      int tmp_ret = OB_SUCCESS;
      if (OB_SUCCESS != (tmp_ret = trans.end(false /*commit*/))) {
        LOG_WARN("fail to abort trans", KR(ret), K(tmp_ret), K(tenant_id));
      }
    }
  }
  return ret;
}

int ObTabletChecksumOperator::delete_tablet_checksum_items(
    ObISQLClient &sql_client,
    const uint64_t tenant_id,
    const int64_t min_snapshot_version)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_UNLIKELY((!is_valid_tenant_id(tenant_id)))
      || (OB_INVALID_VERSION == min_snapshot_version)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(min_snapshot_version));
  } else if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE tenant_id = '%lu' AND compaction_scn <= %lu",
             OB_ALL_TABLET_CHECKSUM_TNAME, tenant_id, (uint64_t)(min_snapshot_version < 0 ? 0 : min_snapshot_version)))) {
    LOG_WARN("fail to assign sql", KR(ret), K(tenant_id), K(min_snapshot_version));
  } else if (OB_FAIL(sql_client.write(tenant_id, sql.ptr(), affected_rows))) {
    LOG_WARN("fail to execute sql", KR(ret), K(sql));
  } else {
    LOG_TRACE("succ to delete tablet checksum items", K(tenant_id), K(min_snapshot_version), K(affected_rows));
  }
  return ret;
}

int ObTabletChecksumOperator::delete_tablet_checksum_items(
    ObISQLClient &sql_client,
    const uint64_t tenant_id,
    ObIArray<ObTabletChecksumItem> &items)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  const int64_t items_cnt = items.count();
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id) || items_cnt < 1)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(items_cnt));
  } else if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE tenant_id = '%lu' and (tablet_id, ls_id, compaction_scn)"
             " IN (", OB_ALL_TABLET_CHECKSUM_TNAME, tenant_id))) {
    LOG_WARN("fail to assign sql", KR(ret), K(tenant_id));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && (i < items_cnt); ++i) {
      const ObTabletChecksumItem &tmp_item = items.at(i);
      if (OB_FAIL(sql.append_fmt(
          "(%ld, %ld, %lu)%s",
          tmp_item.tablet_id_.id(),
          tmp_item.ls_id_.id(),
          (uint64_t)(tmp_item.snapshot_version_ < 0 ? 0 : tmp_item.snapshot_version_),
          (i == items_cnt - 1) ? ")" : ", "))) {
        LOG_WARN("fail to assign sql", KR(ret), K(tenant_id), K(tmp_item));
      }
    }

    if (FAILEDx(sql.append_fmt(") ORDER BY tenant_id, tablet_id, ls_id, compaction_scn"))) {
      LOG_WARN("fail to assign sql string", KR(ret), K(tenant_id), K(items_cnt));
    } else if (OB_FAIL(sql_client.write(tenant_id, sql.ptr(), affected_rows))) {
      LOG_WARN("fail to execute sql", KR(ret), K(sql));
    } else {
      LOG_TRACE("succ to delete tablet checksum items", K(tenant_id), K(affected_rows),
        K(items_cnt));
    }
  }
  return ret;
}

int ObTabletChecksumOperator::load_all_snapshot_versions(
    ObISQLClient &sql_client, 
    const uint64_t tenant_id,
    ObIArray<int64_t> &snapshot_versions)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id));
  } else {
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = nullptr;
      if (OB_FAIL(sql.assign_fmt("SELECT DISTINCT snapshot_version as dis_snapshot FROM %s ORDER BY snapshot_version",
          OB_ALL_TABLET_CHECKSUM_TNAME))) {
        LOG_WARN("fail to append sql", KR(ret), K(tenant_id));
      } else if (OB_FAIL(sql_client.read(res, tenant_id, sql.ptr()))) {
        LOG_WARN("fail to execute sql", KR(ret), K(sql), K(tenant_id));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get sql result", KR(ret), K(sql), K(tenant_id));
      } else {
        while (OB_SUCC(ret)) {
          int64_t snapshot_version = OB_INVALID_VERSION;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END != ret) {
              LOG_WARN("fail to get next row", KR(ret), K(tenant_id));
            }
          } else {
            EXTRACT_UINT_FIELD_MYSQL(*result, "dis_snapshot", snapshot_version, uint64_t);
          }

          if (OB_FAIL(ret)) {
          } else if (OB_UNLIKELY(OB_INVALID_VERSION == snapshot_version)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("invalid snapshot_version", KR(ret), K(snapshot_version), K(tenant_id), K(sql));
          } else if (OB_FAIL(snapshot_versions.push_back(snapshot_version))) {
            LOG_WARN("fail to push back", KR(ret), K(snapshot_version), K(tenant_id));
          }
        } // end for while

        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;  
}

} // namespace share
} // namespace oceanbase
