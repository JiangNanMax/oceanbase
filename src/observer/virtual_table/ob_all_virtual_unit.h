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

#ifndef SRC_OBSERVER_VIRTUAL_TABLE_OB_ALL_VIRTUAL_UNIT_H_
#define SRC_OBSERVER_VIRTUAL_TABLE_OB_ALL_VIRTUAL_UNIT_H_

#include "common/row/ob_row.h"
#include "share/ob_scanner.h"
#include "share/ob_virtual_table_scanner_iterator.h"
#include "observer/omt/ob_tenant_meta.h"

namespace oceanbase
{
namespace observer
{
class ObAllVirtualUnit : public common::ObVirtualTableScannerIterator
{
  enum COLUMN_ID_LIST
  {
    SVR_IP = common::OB_APP_MIN_COLUMN_ID,
    SVR_PORT,
    UNIT_ID,
    TENANT_ID,
    ZONE,
    MIN_CPU,
    MAX_CPU,
    MEMORY_SIZE,
    MIN_IOPS,
    MAX_IOPS,
    IOPS_WEIGHT,
    LOG_DISK_SIZE,
    LOG_DISK_IN_USE,
    DATA_DISK_IN_USE,
    STATUS,
    CREATE_TIME
  };

public:
  ObAllVirtualUnit();
  virtual ~ObAllVirtualUnit();
  int init(common::ObAddr &addr);
  virtual int inner_open();
  virtual void reset();
  virtual int inner_get_next_row(common::ObNewRow *&row);

private:
  char ip_buf_[common::OB_IP_STR_BUFF];
  common::ObAddr addr_;
  int64_t tenant_idx_;
  common::ObArray<omt::ObTenantMeta> tenant_meta_arr_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObAllVirtualUnit);
};

}
}
#endif /* SRC_OBSERVER_VIRTUAL_TABLE_OB_ALL_VIRTUAL_UNIT_H_ */
