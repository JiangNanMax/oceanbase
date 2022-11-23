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

#include "observer/virtual_table/ob_all_virtual_transaction_checkpoint.h"
#include "storage/tx_storage/ob_ls_service.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::storage::checkpoint;
namespace oceanbase
{
namespace observer
{

ObAllVirtualTransCheckpointInfo::ObAllVirtualTransCheckpointInfo()
    : ObVirtualTableScannerIterator(),
      addr_(),
      ls_id_(share::ObLSID::INVALID_LS_ID),
      ls_iter_guard_()
{
}

ObAllVirtualTransCheckpointInfo::~ObAllVirtualTransCheckpointInfo()
{
  reset();
}

void ObAllVirtualTransCheckpointInfo::reset()
{
  omt::ObMultiTenantOperator::reset();
  addr_.reset();
  ObVirtualTableScannerIterator::reset();
}

int ObAllVirtualTransCheckpointInfo::get_next_ls_(ObLS *&ls)
{
  int ret = OB_SUCCESS;

  if (ls_iter_guard_.get_ptr() == nullptr
      && OB_FAIL(MTL(ObLSService*)->get_ls_iter(ls_iter_guard_, ObLSGetMod::OBSERVER_MOD))) {
    SERVER_LOG(WARN, "get_ls_iter fail", K(ret));
  } else if (OB_FAIL(ls_iter_guard_->get_next(ls))) {
    if (OB_ITER_END != ret) {
      SERVER_LOG(WARN, "get_next_ls failed", K(ret));
    }
  } else {
    ls_id_ = ls->get_ls_id().id();
  }

  return ret;
}

int ObAllVirtualTransCheckpointInfo::prepare_to_read_()
{
  int ret = OB_SUCCESS;
  ObLS *ls = nullptr;
  ObArray<ObCommonCheckpointVTInfo> infos;
  ob_common_checkpoint_iter_.reset();
  if (OB_FAIL(get_next_ls_(ls))) {
    if (OB_ITER_END != ret) {
      SERVER_LOG(WARN, "get_next_ls failed", K(ret));
    }
  } else if (NULL == ls) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "ls shouldn't NULL here", K(ret), K(ls));
  } else if (FALSE_IT(infos.reset())) {
  } else if (OB_FAIL(ls->get_common_checkpoint_info(infos))) {
    SERVER_LOG(WARN, "get commoncheckpoint info failed", K(ret), KPC(ls));
  } else {
    int64_t idx = 0;
    for (; idx < infos.count() && OB_SUCC(ret); ++idx) {
      if (OB_FAIL(ob_common_checkpoint_iter_.push(infos.at(idx)))) {
        SERVER_LOG(ERROR, "ob_common_checkpoint_iter push failed", K(ret), KPC(ls));
      }
    }
  }

  if (OB_SUCC(ret) && OB_FAIL(ob_common_checkpoint_iter_.set_ready())) {
    SERVER_LOG(WARN, "iterate common_checkpoint info begin error", K(ret));
  }

  if (OB_FAIL(ret)) {
    ob_common_checkpoint_iter_.reset();
  }

  return ret;
}

int ObAllVirtualTransCheckpointInfo::get_next_(ObCommonCheckpointVTInfo &common_checkpoint)
{
  int ret = OB_SUCCESS;
  // ensure inner_get_next_row can get new data
  bool need_retry = true;
  while (need_retry) {
    if (!ob_common_checkpoint_iter_.is_ready() && OB_FAIL(prepare_to_read_())) {
      if (OB_ITER_END == ret) {
        SERVER_LOG(DEBUG, "iterate commoncheckpoint info iter end", K(ret));
      } else {
        SERVER_LOG(WARN, "prepare data failed", K(ret));
      }
    } else if (OB_FAIL(ob_common_checkpoint_iter_.get_next(common_checkpoint))) {
      if (OB_ITER_END == ret) {
        ob_common_checkpoint_iter_.reset();
        SERVER_LOG(DEBUG, "iterate commoncheckpoint info iter in the ls end",
                                                              K(ret), K(ls_id_));
        continue;
      } else {
        SERVER_LOG(WARN, "get next commoncheckpoint info error.", K(ret));
      }
    }
    need_retry = false;
  }
  return ret;
}

bool ObAllVirtualTransCheckpointInfo::is_need_process(uint64_t tenant_id)
{
  if (is_sys_tenant(effective_tenant_id_) || tenant_id == effective_tenant_id_) {
    return true;
  }
  return false;
}

void ObAllVirtualTransCheckpointInfo::release_last_tenant()
{
  ls_id_ = share::ObLSID::INVALID_LS_ID;
  ls_iter_guard_.reset();
  ob_common_checkpoint_iter_.reset();
}

int ObAllVirtualTransCheckpointInfo::inner_get_next_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(execute(row))) {
    SERVER_LOG(WARN, "execute fail", K(ret));
  }
  return ret;
}

int ObAllVirtualTransCheckpointInfo::process_curr_tenant(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  ObCommonCheckpointVTInfo common_checkpoint;
  if (NULL == allocator_) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "allocator_ shouldn't be NULL", K(allocator_), K(ret));
  } else if (OB_FAIL(get_next_(common_checkpoint))) {
    if (OB_ITER_END != ret) {
      SERVER_LOG(WARN, "get_next_common_checkpoint failed", K(ret));
    }
  } else {
    const int64_t col_count = output_column_ids_.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      uint64_t col_id = output_column_ids_.at(i);
      switch (col_id) {
        case OB_APP_MIN_COLUMN_ID:
          // svr_ip
          if (addr_.ip_to_string(ip_buf_, sizeof(ip_buf_))) {
            cur_row_.cells_[i].set_varchar(ip_buf_);
            cur_row_.cells_[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          } else {
            ret = OB_ERR_UNEXPECTED;
            SERVER_LOG(WARN, "fail to execute ip_to_string", K(ret));
          }
          break;
        case OB_APP_MIN_COLUMN_ID + 1:
          // svr_port
          cur_row_.cells_[i].set_int(addr_.get_port());
          break;
        case OB_APP_MIN_COLUMN_ID + 2:
          // tenant_id
          cur_row_.cells_[i].set_int(MTL_ID());
          break;
        case OB_APP_MIN_COLUMN_ID + 3:
          // ls_id
          cur_row_.cells_[i].set_int(ls_id_);
          break;
        case OB_APP_MIN_COLUMN_ID + 4: {
          cur_row_.cells_[i].set_int(common_checkpoint.tablet_id.id());
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 5: {
          cur_row_.cells_[i].set_uint64((common_checkpoint.rec_log_ts < 0 ? 0 : common_checkpoint.rec_log_ts));
          break;
        }
        case OB_APP_MIN_COLUMN_ID + 6:
          if (OB_FAIL(common_checkpoint_type_to_string(ObCommonCheckpointType(common_checkpoint.checkpoint_type),
                                                       checkpoint_type_buf_,
                                                       sizeof(checkpoint_type_buf_)))) {
            SERVER_LOG(WARN, "get common_checkpoint type buf failed", K(ret), K(common_checkpoint));
          } else {
            checkpoint_type_buf_[MAX_CHECKPOINT_TYPE_BUF_LENGTH - 1] = '\0';
            cur_row_.cells_[i].set_varchar(checkpoint_type_buf_);
            cur_row_.cells_[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
          }
          break;
        case OB_APP_MIN_COLUMN_ID + 7:
          cur_row_.cells_[i].set_int(common_checkpoint.is_flushing ? 1 : 0);
          break;
        default:
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "invalid col_id", K(ret), K(col_id));
          break;
      }
    }
  }
  if (OB_SUCC(ret)) {
    row = &cur_row_;
  }

  return ret;
}

} // observer
} // oceanbase
