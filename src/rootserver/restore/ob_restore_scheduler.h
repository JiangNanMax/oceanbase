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

#ifndef OCEANBASE_ROOTSERVER_OB_RESTORE_SCHEDULER_H_
#define OCEANBASE_ROOTSERVER_OB_RESTORE_SCHEDULER_H_

#include "share/backup/ob_backup_info_mgr.h"
#include "rootserver/restore/ob_restore_util.h"
#include "rootserver/ob_primary_ls_service.h"//ObTenantThreadHelper
#include "share/backup/ob_backup_struct.h"
#include "share/ob_rpc_struct.h"
#include "share/ob_common_rpc_proxy.h"
#include "share/ob_rpc_struct.h"
#include "share/ob_upgrade_utils.h"

namespace oceanbase
{
namespace share
{
class ObLSTableOperator;
struct ObLSAttr;
struct ObHisRestoreJobPersistInfo;
}
namespace rootserver
{
// Running in a single thread.
// schedule restore job, register to sys ls of meta tenant
class ObRestoreService : public ObTenantThreadHelper,
  public logservice::ObICheckpointSubHandler, public logservice::ObIReplaySubHandler,
  public share::ObCheckStopProvider
{
public:
  static const int64_t MAX_RESTORE_TASK_CNT = 10000;
public:
  ObRestoreService();
  virtual ~ObRestoreService();
  static int mtl_init(ObRestoreService *&ka);
  int init(share::schema::ObMultiVersionSchemaService *schema_service,
           common::ObMySQLProxy *sql_proxy,
           obrpc::ObCommonRpcProxy *rpc_proxy,
           obrpc::ObSrvRpcProxy *srv_rpc_proxy,
           share::ObLSTableOperator *lst_operator,
           const common::ObAddr &self_addr);
  virtual void do_work() override;
  void destroy();
public:
  virtual int64_t get_rec_log_ts() override { return INT64_MAX;}
  virtual int flush(int64_t rec_log_ts) override { return OB_SUCCESS; }
  int replay(const void *buffer, const int64_t nbytes, const palf::LSN &lsn, const int64_t ts_ns) override
  {
    UNUSED(buffer);
    UNUSED(nbytes);
    UNUSED(lsn);
    UNUSED(ts_ns);
    return OB_SUCCESS;
  }
  enum TenantRestoreStatus
  {
    IN_PROGRESS = 0,
    SUCCESS,
    FAILED
  };
  bool is_tenant_restore_finish(const TenantRestoreStatus tenant_restore_status) const
  {
    return SUCCESS == tenant_restore_status || FAILED == tenant_restore_status;
  }
  bool is_tenant_restore_success(const TenantRestoreStatus tenant_restore_status) const
  {
    return SUCCESS == tenant_restore_status;
  }
  bool is_tenant_restore_failed(const TenantRestoreStatus tenant_restore_status) const
  {
    return FAILED == tenant_restore_status;
  }


public:
  static int assign_pool_list(const char *str,
                       common::ObIArray<common::ObString> &pool_list);

private:
  int idle();
  int check_stop() const override;

  int process_restore_job(const share::ObPhysicalRestoreJob &job);
  int process_sys_restore_job(const share::ObPhysicalRestoreJob &job);
  int try_recycle_job(const share::ObPhysicalRestoreJob &job);

  int restore_tenant(const share::ObPhysicalRestoreJob &job_info);
  int restore_upgrade(const share::ObPhysicalRestoreJob &job_info);
  int restore_pre(const share::ObPhysicalRestoreJob &job_info);

  int post_check(const share::ObPhysicalRestoreJob &job_info);
  int restore_finish(const share::ObPhysicalRestoreJob &job_info);
  int tenant_restore_finish(const share::ObPhysicalRestoreJob &job_info);
  int restore_init_ls(const share::ObPhysicalRestoreJob &job_info);
  int restore_wait_ls_finish(const share::ObPhysicalRestoreJob &job_info);
  int restore_wait_tenant_finish(const share::ObPhysicalRestoreJob &job_info);

  int fill_create_tenant_arg(const share::ObPhysicalRestoreJob &job_info,
                             const ObSqlString &pool_list,
                             obrpc::ObCreateTenantArg &arg);
  int convert_parameters(const share::ObPhysicalRestoreJob &job_info);

  int check_locality_valid(const share::schema::ZoneLocalityIArray &locality);
  int try_update_job_status(
      int return_ret,
      const share::ObPhysicalRestoreJob &job,
      share::PhysicalRestoreMod mod = share::PHYSICAL_RESTORE_MOD_RS);
  void record_rs_event(const share::ObPhysicalRestoreJob &job,
                       const share::PhysicalRestoreStatus next_status);
  share::PhysicalRestoreStatus get_next_status(int return_ret, share::PhysicalRestoreStatus current_status);
  share::PhysicalRestoreStatus get_sys_next_status(share::PhysicalRestoreStatus current_status);
  
  int fill_restore_statistics(const share::ObPhysicalRestoreJob &job_info);
private:
  int create_all_ls_(const share::schema::ObTenantSchema &tenant_schema,
      const common::ObIArray<share::ObLSAttr> &ls_attr_array);
  int wait_all_ls_created_(const share::schema::ObTenantSchema &tenant_schema,
      const share::ObPhysicalRestoreJob &job);
  int finish_create_ls_(const share::schema::ObTenantSchema &tenant_schema,
      const common::ObIArray<share::ObLSAttr> &ls_attr_array);
  int check_all_ls_restore_finish_(const uint64_t tenant_id, TenantRestoreStatus &tenant_restore_status);
  int try_get_tenant_restore_history_(const share::ObPhysicalRestoreJob &job_info,
                                       share::ObHisRestoreJobPersistInfo &history_info);
  int check_tenant_can_restore_(const uint64_t tenant_id);
  int reset_schema_status_(const uint64_t tenant_id);
private:
  bool inited_;
  share::schema::ObMultiVersionSchemaService *schema_service_;
  common::ObMySQLProxy *sql_proxy_;
  obrpc::ObCommonRpcProxy *rpc_proxy_;
  obrpc::ObSrvRpcProxy *srv_rpc_proxy_;
  share::ObLSTableOperator *lst_operator_;
  share::ObUpgradeProcesserSet upgrade_processors_;
  common::ObAddr self_addr_;
  uint64_t tenant_id_;
  int64_t idle_time_us_;
  DISALLOW_COPY_AND_ASSIGN(ObRestoreService);
};

} // end namespace rootserver
} // end namespace oceanbase

#endif // OCEANBASE_ROOTSERVER_OB_RESTORE_SCHEDULER_H_
