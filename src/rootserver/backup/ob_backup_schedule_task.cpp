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

#define USING_LOG_PREFIX RS

#include "ob_backup_task_scheduler.h"
#include "share/location_cache/ob_location_service.h"
#include "share/backup/ob_backup_data_table_operator.h"
#include "share/backup/ob_backup_clean_operator.h"
#include "share/ob_srv_rpc_proxy.h"
#include "share/ls/ob_ls_table_operator.h"
namespace oceanbase 
{
using namespace common;
using namespace lib;
using namespace obrpc;
using namespace share;
namespace rootserver 
{
// ObBackupServerStatKey
bool ObBackupServerStatKey::is_valid() const
{
  return type_ < BackupJobType::BACKUP_JOB_MAX 
         && type_ >= BackupJobType::BACKUP_DATA_JOB && addr_.is_valid();
}

bool ObBackupServerStatKey::operator==(const ObBackupServerStatKey &that) const
{
  return type_ == that.type_ && addr_ == that.addr_;
}

ObBackupServerStatKey &ObBackupServerStatKey::operator=(const ObBackupServerStatKey &that)
{
  type_ = that.type_;
  addr_ = that.addr_;
  hash_value_ = that.hash_value_;
  return (*this);
}

uint64_t ObBackupServerStatKey::hash() const
{
  return hash_value_;
}

int ObBackupServerStatKey::init(const common::ObAddr &addr, const BackupJobType type)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(type >= BackupJobType::BACKUP_JOB_MAX 
                  || type < BackupJobType::BACKUP_DATA_JOB
                  || !addr.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(type), K(addr));
  } else {
    type_ = type;
    addr_ = addr;
    hash_value_ = inner_hash();
  }
  return ret;
}

int ObBackupServerStatKey::init(const ObBackupServerStatKey &that)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!that.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else {
    type_ = that.type_;
    addr_ = that.addr_;
    hash_value_ = inner_hash();
  }
  return ret;
}

uint64_t ObBackupServerStatKey::inner_hash() const
{
  uint64_t hash_val = 0;
  hash_val = addr_.hash();
  hash_val = murmurhash(&type_, sizeof(type_), hash_val);
  return hash_val;
}

// ObBackupScheduleTaskKey
bool ObBackupScheduleTaskKey::is_valid() const
{
  return type_ < BackupJobType::BACKUP_JOB_MAX 
         && type_ >= BackupJobType::BACKUP_DATA_JOB;
}

bool ObBackupScheduleTaskKey::operator==(const ObBackupScheduleTaskKey &that) const
{
  return ls_id_ == that.ls_id_ 
	       && task_id_ == that.task_id_ 
         && tenant_id_ == that.tenant_id_ 
         && job_id_ == that.job_id_ 
         && type_ == that.type_;
}

ObBackupScheduleTaskKey &ObBackupScheduleTaskKey::operator=(const ObBackupScheduleTaskKey &that)
{
  ls_id_ = that.ls_id_;
	task_id_ = that.task_id_;
  tenant_id_ = that.tenant_id_;
  job_id_ = that.job_id_;
  type_ = that.type_;
  hash_value_ = that.hash_value_;
  return (*this);
}

uint64_t ObBackupScheduleTaskKey::hash() const
{
  return hash_value_;
}

int ObBackupScheduleTaskKey::init(
    const uint64_t tenant_id, 
    const uint64_t job_id, 
    const uint64_t task_id, 
    const uint64_t key_1,
    const BackupJobType key_type)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(key_type >= BackupJobType::BACKUP_JOB_MAX 
                  || key_type < BackupJobType::BACKUP_DATA_JOB)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(key_type));
  } else {
    tenant_id_ = tenant_id;
    job_id_ = job_id;
    ls_id_ = key_1;
    task_id_ = task_id;
    type_ = key_type;
    hash_value_ = inner_hash();
  }
  return ret;
}

int ObBackupScheduleTaskKey::init(const ObBackupScheduleTaskKey &that)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!that.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else {
    tenant_id_ = that.tenant_id_;
    job_id_ = that.job_id_;
    ls_id_ = that.ls_id_;
    task_id_ = that.task_id_;
    type_ = that.type_;
    hash_value_ = inner_hash();
  }
  return ret;
}

uint64_t ObBackupScheduleTaskKey::inner_hash() const
{
  uint64_t hash_val = 0;
  hash_val = murmurhash(&tenant_id_, sizeof(tenant_id_), hash_val);
  hash_val = murmurhash(&job_id_, sizeof(job_id_), hash_val);
  hash_val = murmurhash(&task_id_, sizeof(task_id_), hash_val);
  hash_val = murmurhash(&ls_id_, sizeof(ls_id_), hash_val);
  hash_val = murmurhash(&type_, sizeof(type_), hash_val);
  return hash_val;
}

// ObBackupScheduleTask
int ObBackupScheduleTask::set_schedule(const common::ObAddr &server)
{
  int ret = OB_SUCCESS;
  int64_t now = ObTimeUtility::current_time();
  if (!server.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(server));
  } else {
    set_dst(server);
    set_schedule_time(now);
  }
  return ret;
}

void ObBackupScheduleTask::clear_schedule() 
{
  set_schedule_time(0);
  dst_.reset();
}

int ObBackupScheduleTask::deep_copy(const ObBackupScheduleTask &that)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(optional_servers_.assign(that.optional_servers_))) {
    LOG_WARN("deep copy optional servers failed", K(ret), K(optional_servers_));
  } else {
    task_key_ = that.task_key_;
    trace_id_ = that.trace_id_;
    dst_ = that.dst_;
    status_ = that.status_;
    generate_time_ = that.generate_time_;
    schedule_time_ = that.schedule_time_;
    executor_time_ = that.executor_time_;
  }
  return ret;
}

int ObBackupScheduleTask::set_optional_servers(const ObIArray<share::ObBackupServer> &servers)
{
  int ret = OB_SUCCESS;
  if (servers.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else if (OB_FAIL(append(optional_servers_, servers))) {
    LOG_WARN("failed to append optional servers", K(ret));
  }
#ifdef ERRSIM
  if (OB_SUCC(ret)) {
    const ObString &errsim_server = GCONF.backup_errsim_task_server_addr.str();
    ObBackupServer backup_server;
    backup_server.priority_ = 0;
    if (!errsim_server.empty()) {
      common::ObAddr addr;
      if (OB_FAIL(addr.parse_from_string(errsim_server))) {
        LOG_WARN("failed to parse from string", K(ret));
      } else {
        optional_servers_.reset();
        backup_server.server_ = addr;
        if (OB_FAIL(optional_servers_.push_back(backup_server))) {
          LOG_WARN("failed to push back", K(ret));
        }
      }
      LOG_INFO("errsim set optional server", K(errsim_server), K(addr), K(backup_server));
    }
  }
#endif
  return ret;
}

int ObBackupScheduleTask::update_dst_and_doing_status(common::ObMySQLProxy &sql_proxy)
{
  int ret = OB_SUCCESS;
  ObLSID ls_id(get_ls_id());
  ObCurTraceId::init(GCONF.self_addr_);
  share::ObTaskId trace_id(*ObCurTraceId::get_trace_id());
  if (OB_FAIL(do_update_dst_and_doing_status_(sql_proxy, dst_, trace_id))) {
    LOG_WARN("fail to do update task dst and advance task status to doing", K(ret));
  } else {
    trace_id_ = trace_id;
    status_.status_ = ObBackupTaskStatus::Status::DOING;
    LOG_INFO("succeed update task dst and advance task status to doing", K(*this));
  }
  return ret;
}

int ObBackupScheduleTask::build(const ObBackupScheduleTaskKey & key, const share::ObTaskId &trace_id, 
    const share::ObBackupTaskStatus &status, const common::ObAddr &dst)
{
  int ret = OB_SUCCESS;
  if (!key.is_valid() || !status.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(key), K(status));
  } else if (OB_FAIL(task_key_.init(key))) {
    LOG_WARN("fail to init task key", K(ret), K(key));
  } else {
    trace_id_ = trace_id;
    dst_ = dst;
    if (status.is_init() || status.is_pending()) {
      status_.status_ = ObBackupTaskStatus::Status::PENDING;
    } else if (status.is_doing()) {
      status_.status_ = ObBackupTaskStatus::Status::DOING;
    }
    set_generate_time(ObTimeUtility::current_time());
  }
  return ret;
}

int ObBackupScheduleTask::build_from_res(const obrpc::ObBackupTaskRes &res, const BackupJobType &type)
{
  int ret = OB_SUCCESS;
  ObBackupScheduleTaskKey key;
  if (!res.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(res));
  } else if (OB_FAIL(key.init(res.tenant_id_, res.job_id_, res.task_id_, res.ls_id_.id(), type))) {
    LOG_WARN("failed to init backup schedule task key", K(ret), K(res));
  } else if (OB_FAIL(init_task_key(key))) {
    LOG_WARN("failed to init task key", K(key));
  } else {
    set_dst(res.src_server_);
    set_trace_id(res.trace_id_);
  }
  return ret;
}

/*
 *---------------------ObBackupDataLSTask----------------------
 */

ObBackupDataLSTask::ObBackupDataLSTask()
  : incarnation_id_(OB_BACKUP_INVALID_INCARNATION_ID),
    backup_set_id_(OB_BACKUP_INVALID_BACKUP_SET_ID),
    backup_type_(),
    backup_date_(OB_INVALID_TIMESTAMP),
    ls_id_(),
    turn_id_(OB_BACKUP_INVALID_TURN_ID),
    retry_id_(OB_BACKUP_INVALID_RETRY_ID),
    start_scn_(OB_INVALID_TIMESTAMP),
    backup_path_(),
    backup_status_()
{
}

ObBackupDataLSTask::~ObBackupDataLSTask()
{
}

int ObBackupDataLSTask::clone(void *input_ptr, ObBackupScheduleTask *&out_task) const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(input_ptr)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(input_ptr));
  } else {
    ObBackupDataLSTask *my_task = new (input_ptr) ObBackupDataLSTask();
    if (OB_ISNULL(my_task)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("taks is nullptr", K(ret));
    } else if (OB_FAIL(my_task->ObBackupScheduleTask::deep_copy(*this))) {
      LOG_WARN("fail to deep copy base task", K(ret));
    } else if (OB_FAIL(my_task->backup_path_.assign(backup_path_))) {
      LOG_WARN("failed to assign backup dest", K(ret));
    } else {
      my_task->incarnation_id_ = incarnation_id_;
      my_task->backup_set_id_ = backup_set_id_;
      my_task->backup_type_.type_ = backup_type_.type_;
      my_task->backup_date_ = backup_date_;
      my_task->ls_id_ = ls_id_;
      my_task->turn_id_ = turn_id_;
      my_task->retry_id_ = retry_id_;
      my_task->start_scn_ = start_scn_;
      my_task->backup_status_.status_ = backup_status_.status_;
    }
    if (OB_SUCC(ret)) {
      out_task = my_task;
    } else if (OB_NOT_NULL(my_task)) {
      my_task->~ObBackupDataLSTask();
      my_task = nullptr;
    }
  }
  return ret;
}

int64_t ObBackupDataLSTask::get_deep_copy_size() const
{
  return sizeof(ObBackupDataLSTask);
}

bool ObBackupDataLSTask::can_execute_on_any_server() const
{
  return false;
}

int ObBackupDataLSTask::do_update_dst_and_doing_status_(common::ObMySQLProxy &sql_proxy, common::ObAddr &dst, share::ObTaskId &trace_id) 
{
  int ret = OB_SUCCESS;
  ObLSID ls_id(get_ls_id());
  if (OB_FAIL(ObBackupLSTaskOperator::update_dst_and_status(sql_proxy, get_task_id(), get_tenant_id(), ls_id, trace_id, dst))) {
    LOG_WARN("failed to update task status", K(ret), K(*this), K(dst));
  }
  return ret;
}

int ObBackupDataLSTask::execute(obrpc::ObSrvRpcProxy &rpc_proxy) const
{
  int ret = OB_SUCCESS;
  obrpc::ObBackupDataArg arg;
  arg.trace_id_ = get_trace_id();
  arg.tenant_id_ = get_tenant_id();
  arg.task_id_ = get_task_id();
  arg.backup_set_id_ = backup_set_id_;
  arg.incarnation_id_ = incarnation_id_;
  arg.backup_type_ = backup_type_.type_;
  arg.backup_date_ = backup_date_;
  arg.ls_id_ = ls_id_;
  arg.turn_id_ = turn_id_;
  arg.retry_id_ = retry_id_;
  arg.dst_server_ = get_dst();
  arg.job_id_ = get_job_id();
  if (OB_FAIL(backup_status_.get_backup_data_type(arg.backup_data_type_))) {
    LOG_WARN("failed to get backup data type", K(ret), K_(backup_status));
  } else if (OB_FAIL(arg.backup_path_.assign(backup_path_))) {
    LOG_WARN("failed to assign backup dest", K(ret), K(backup_path_));
  } else if (OB_FAIL(rpc_proxy.to(get_dst()).backup_ls_data(arg))) {
    LOG_WARN("fail to send backup ls data task", K(ret), K(arg));
  } else {
    LOG_INFO("start to backup ls data", K(arg));
  }
  return ret;
}

int ObBackupDataLSTask::cancel(obrpc::ObSrvRpcProxy &rpc_proxy) const
{
  int ret = OB_SUCCESS;
  obrpc::ObCancelTaskArg rpc_arg;
  rpc_arg.task_id_ = get_trace_id();
  if (OB_FAIL(rpc_proxy.to(get_dst()).cancel_sys_task(rpc_arg))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      LOG_INFO("task may not excute on server", K(rpc_arg), "dst", get_dst());
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("failed to cancel sys task", K(ret), K(rpc_arg));
    }
  }
  return ret;
}

int ObBackupDataLSTask::build(const ObBackupJobAttr &job_attr, const ObBackupSetTaskAttr &set_task_attr, const ObBackupLSTaskAttr &ls_attr)
{
  int ret = OB_SUCCESS;
  ObBackupScheduleTaskKey key;
  if (!job_attr.is_valid() || !ls_attr.is_valid()) {
    ret = OB_SUCCESS;
    LOG_WARN("invalid argument", K(ret), K(job_attr), K(ls_attr));
  } else if (OB_FAIL(key.init(ls_attr.tenant_id_, job_attr.job_id_, ls_attr.task_id_, ls_attr.ls_id_.id(), BackupJobType::BACKUP_DATA_JOB))) {
    LOG_WARN("failed to init backup schedule task key", K(ret), K(job_attr), K(ls_attr));
  } else if (OB_FAIL(ObBackupScheduleTask::build(key, ls_attr.task_trace_id_, ls_attr.status_, ls_attr.dst_))) {
    LOG_WARN("fail to build backup schedule task", K(ret), "trace_id", ls_attr.task_trace_id_, "status", ls_attr.status_, "dst", ls_attr.dst_);
  } else {
    incarnation_id_ = job_attr.incarnation_id_;
    backup_set_id_ = ls_attr.backup_set_id_;
    backup_type_.type_ = ls_attr.backup_type_.type_;
    backup_date_ = ls_attr.backup_date_;
    ls_id_ = ls_attr.ls_id_;
    turn_id_ = ls_attr.turn_id_;
    retry_id_ = ls_attr.retry_id_;
    start_scn_ = ls_attr.ls_id_.is_sys_ls() ? set_task_attr.start_scn_ : set_task_attr.user_ls_start_scn_;
    backup_status_.status_ = set_task_attr.status_.status_;
    if (OB_FAIL(backup_path_.assign(job_attr.backup_path_))) {
      LOG_WARN("failed to assign backup dest", K(ret), K(job_attr.backup_path_));
    } else if (OB_FAIL(set_optional_servers_(ls_attr.black_servers_))) {
      LOG_WARN("failed to set optional servers", K(ret), K(ls_attr));
    }
  }
  return ret;
}

int ObBackupDataLSTask::set_optional_servers_(const ObIArray<common::ObAddr> &black_servers)
{
  int ret = OB_SUCCESS;
  ObLSInfo ls_info;
  ObArray<ObBackupServer> servers;
  uint64_t tenant_id = get_tenant_id();
  share::ObLSTableOperator *lst_operator = GCTX.lst_operator_;
  int64_t cluster_id = GCONF.cluster_id;
  if (nullptr == lst_operator) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("lst_operator ptr is null", K(ret));
  } else if (OB_FAIL(lst_operator->get(cluster_id, tenant_id, ls_id_, ls_info))) {
    LOG_WARN("failed to get log stream info", K(ret), K(cluster_id), K(tenant_id), K(ls_id_));
  } else {
    const ObLSInfo::ReplicaArray &replica_array = ls_info.get_replicas();
    for (int i = 0; OB_SUCC(ret) && i < replica_array.count(); ++i) {
      const ObLSReplica &replica = replica_array.at(i);
      if (replica.is_in_service() && !replica.is_strong_leader() && replica.is_valid() 
          && !check_replica_in_black_server_(replica, black_servers)) { 
        ObBackupServer server;
        server.set(replica.get_server(), 0/*high priority*/);
        if (OB_FAIL(servers.push_back(server))) {
          LOG_WARN("failed to push server", K(ret), K(server));
        }
      }
    }
    for (int i = 0; OB_SUCC(ret) && i < replica_array.count(); ++i) {
      const ObLSReplica &replica = replica_array.at(i);
      if (replica.is_in_service() && replica.is_strong_leader() && replica.is_valid()
          && (replica_array.count() == 1 || !check_replica_in_black_server_(replica, black_servers))) {
        // if only has one replica. no use black server.
        ObBackupServer server;
        server.set(replica.get_server(), 1/*low priority*/);
        if (OB_FAIL(servers.push_back(server))) {
          LOG_WARN("failed to push server", K(ret), K(server));
        }
      }
    }
    if (OB_SUCC(ret) && servers.empty()) {
      ret = OB_EAGAIN;
      LOG_WARN("no optional servers, retry_later", K(ret), K(*this));
    }
  }

  if (OB_SUCC(ret) && OB_FAIL(set_optional_servers(servers))) {
    LOG_WARN("failed to optional servers", K(ret));
  } else {
    FLOG_INFO("task optional servers are：", K(*this), K(servers));
  }
  return ret;
}

bool ObBackupDataLSTask::check_replica_in_black_server_(const ObLSReplica &replica, const ObIArray<common::ObAddr> &black_servers)
{
  bool is_in_black_servers = false;
  for (int i = 0; i < black_servers.count(); ++i) {
    const ObAddr &server = black_servers.at(i);
    if (server == replica.get_server()) {
      is_in_black_servers = true; 
    }
  }
  return is_in_black_servers;
}

/*
 *-------------------------ObBackupComplLogTask------------------------------
 */

ObBackupComplLogTask::ObBackupComplLogTask()
  : incarnation_id_(OB_BACKUP_INVALID_INCARNATION_ID),
    backup_set_id_(OB_BACKUP_INVALID_BACKUP_SET_ID),
    backup_type_(),
    backup_date_(OB_INVALID_TIMESTAMP),
    ls_id_(),
    start_scn_(OB_INVALID_TIMESTAMP),
    end_scn_(OB_INVALID_TIMESTAMP),
    backup_path_(),
    backup_status_()
{
}

ObBackupComplLogTask::~ObBackupComplLogTask()
{
}

int ObBackupComplLogTask::clone(void *input_ptr, ObBackupScheduleTask *&out_task) const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(input_ptr)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(input_ptr));
  } else {
    ObBackupComplLogTask *my_task = new (input_ptr) ObBackupComplLogTask();
    if (OB_ISNULL(my_task)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("taks is nullptr", K(ret));
    } else if (OB_FAIL(my_task->ObBackupScheduleTask::deep_copy(*this))) {
      LOG_WARN("fail to deep copy base task", K(ret));
    } else if (OB_FAIL(my_task->backup_path_.assign(backup_path_))) {
      LOG_WARN("failed to assign backup dest", K(ret));
    } else {
      my_task->incarnation_id_ = incarnation_id_;
      my_task->backup_set_id_ = backup_set_id_;
      my_task->backup_type_.type_ = backup_type_.type_;
      my_task->backup_date_ = backup_date_;
      my_task->ls_id_ = ls_id_;
      my_task->start_scn_ = start_scn_;
      my_task->end_scn_ = end_scn_;
      my_task->backup_status_.status_ = backup_status_.status_;
    }
    if (OB_SUCC(ret)) {
      out_task = my_task;
    } else if (OB_NOT_NULL(my_task)) {
      my_task->~ObBackupComplLogTask();
      my_task = nullptr;
    }
  }
  return ret;
}
int64_t ObBackupComplLogTask::get_deep_copy_size() const
{
  return sizeof(ObBackupComplLogTask);
}

bool ObBackupComplLogTask::can_execute_on_any_server() const
{
  return true;
}

int ObBackupComplLogTask::do_update_dst_and_doing_status_(common::ObMySQLProxy &sql_proxy, common::ObAddr &dst, share::ObTaskId &trace_id) 
{
  int ret = OB_SUCCESS;
  ObLSID ls_id(get_ls_id());
  if (OB_FAIL(ObBackupLSTaskOperator::update_dst_and_status(sql_proxy, get_task_id(), get_tenant_id(), ls_id, trace_id, dst))) {
    LOG_WARN("failed to update task status", K(ret), K(dst), K(trace_id));
  } 
  return ret;
}
int ObBackupComplLogTask::execute(obrpc::ObSrvRpcProxy &rpc_proxy) const
{
  int ret = OB_SUCCESS;
  obrpc::ObBackupComplLogArg arg;
  arg.trace_id_ = get_trace_id();
  arg.job_id_ = get_job_id();
  arg.tenant_id_ = get_tenant_id();
  arg.task_id_ = get_task_id();
  arg.backup_set_id_ = backup_set_id_;
  arg.incarnation_id_ = incarnation_id_;
  arg.backup_date_ = backup_date_;
  arg.ls_id_ = ls_id_;
  arg.dst_server_ = get_dst();
  arg.start_scn_ = start_scn_;
  arg.end_scn_ = end_scn_;
  arg.backup_type_ = backup_type_.type_;
  if (OB_FAIL(arg.backup_path_.assign(backup_path_))) {
    LOG_WARN("failed to assign backup dest", K(ret), K(backup_path_));
  } else if (OB_FAIL(rpc_proxy.to(get_dst()).backup_completing_log(arg))) {
    LOG_WARN("fail to send backup ls data task", K(ret), K(arg));
  } else {
    LOG_INFO("start to backup completing log", K(arg));
  }
  return ret;
}

int ObBackupComplLogTask::cancel(obrpc::ObSrvRpcProxy &rpc_proxy) const
{
  int ret = OB_SUCCESS;
  obrpc::ObCancelTaskArg rpc_arg;
  rpc_arg.task_id_ = get_trace_id();
  if (OB_FAIL(rpc_proxy.to(get_dst()).cancel_sys_task(rpc_arg))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      LOG_INFO("task may not excute on server", K(rpc_arg), "dst", get_dst());
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("failed to cancel sys task", K(ret), K(rpc_arg));
    }
  }
  return ret;
}

int ObBackupComplLogTask::build(const ObBackupJobAttr &job_attr, const ObBackupSetTaskAttr &set_task_attr, const ObBackupLSTaskAttr &ls_attr)
{
  int ret = OB_SUCCESS;
  ObBackupScheduleTaskKey key;
  if (!job_attr.is_valid() || !ls_attr.is_valid()) {
    ret = OB_SUCCESS;
    LOG_WARN("invalid argument", K(ret), K(job_attr), K(ls_attr));
  } else if (OB_FAIL(key.init(ls_attr.tenant_id_, job_attr.job_id_, ls_attr.task_id_, ls_attr.ls_id_.id(), BackupJobType::BACKUP_DATA_JOB))) {
    LOG_WARN("failed to init backup schedule task key", K(ret), K(job_attr), K(ls_attr));
  } else if (OB_FAIL(ObBackupScheduleTask::build(key, ls_attr.task_trace_id_, ls_attr.status_, ls_attr.dst_))) {
    LOG_WARN("fail to build backup schedule task", K(ret), "trace_id", ls_attr.task_trace_id_, "status", ls_attr.status_, "dst", ls_attr.dst_);
  } else {
    incarnation_id_ = job_attr.incarnation_id_;
    backup_set_id_ = ls_attr.backup_set_id_;
    backup_type_.type_ = ls_attr.backup_type_.type_;
    backup_date_ = ls_attr.backup_date_;
    ls_id_ = ls_attr.ls_id_;
    start_scn_ = set_task_attr.start_scn_;
    end_scn_ = set_task_attr.end_scn_;
    backup_status_.status_ = set_task_attr.status_.status_;
    if (OB_FAIL(backup_path_.assign(job_attr.backup_path_))) {
      LOG_WARN("failed to assign backup dest", K(ret), "backup dest", job_attr.backup_path_);
    } 
  }
  return ret;
}

/*
 *------------------------ObBackupBuildIndexTask--------------------------
 */

ObBackupBuildIndexTask::ObBackupBuildIndexTask()
  : incarnation_id_(OB_BACKUP_INVALID_INCARNATION_ID),
    backup_set_id_(OB_BACKUP_INVALID_BACKUP_SET_ID),
    backup_type_(),
    backup_date_(OB_INVALID_TIMESTAMP),
    turn_id_(OB_BACKUP_INVALID_TURN_ID),
    retry_id_(OB_BACKUP_INVALID_RETRY_ID),
    start_turn_id_(OB_BACKUP_INVALID_TURN_ID),
    backup_path_(),
    backup_status_()
{
}

ObBackupBuildIndexTask::~ObBackupBuildIndexTask()
{
}

int ObBackupBuildIndexTask::clone(void *input_ptr, ObBackupScheduleTask *&out_task) const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(input_ptr)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(input_ptr));
  } else {
    ObBackupBuildIndexTask *my_task = new (input_ptr) ObBackupBuildIndexTask();
    if (OB_ISNULL(my_task)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("taks is nullptr", K(ret));
    } else if (OB_FAIL(my_task->ObBackupScheduleTask::deep_copy(*this))) {
      LOG_WARN("fail to deep copy base task", K(ret));
    } else if (OB_FAIL(my_task->backup_path_.assign(backup_path_))) {
      LOG_WARN("failed to assign backup dest", K(ret));
    } else {
      my_task->incarnation_id_ = incarnation_id_;
      my_task->backup_set_id_ = backup_set_id_;
      my_task->backup_type_.type_ = backup_type_.type_;
      my_task->backup_date_ = backup_date_;
      my_task->turn_id_ = turn_id_;
      my_task->retry_id_ = retry_id_;
      my_task->backup_status_.status_ = backup_status_.status_;
    }
    if (OB_SUCC(ret)) {
      out_task = my_task;
    } else if (OB_NOT_NULL(my_task)) {
      my_task->~ObBackupBuildIndexTask();
      my_task = nullptr;
    }
  }
  return ret;
}
int64_t ObBackupBuildIndexTask::get_deep_copy_size() const
{
  return sizeof(ObBackupBuildIndexTask);
}

bool ObBackupBuildIndexTask::can_execute_on_any_server() const
{
  return false;
}

int ObBackupBuildIndexTask::do_update_dst_and_doing_status_(common::ObMySQLProxy &sql_proxy, common::ObAddr &dst, share::ObTaskId &trace_id) 
{
  int ret = OB_SUCCESS;
  ObLSID ls_id(get_ls_id());
  if (OB_FAIL(ObBackupLSTaskOperator::update_dst_and_status(sql_proxy, get_task_id(), get_tenant_id(), ls_id, trace_id, dst))) {
    LOG_WARN("failed to update task status", K(ret), K(*this), K(dst));
  }
  return ret;
}

int ObBackupBuildIndexTask::execute(obrpc::ObSrvRpcProxy &rpc_proxy) const
{
  int ret = OB_SUCCESS;
  obrpc::ObBackupBuildIdxArg arg;
  arg.job_id_ = get_job_id();
  arg.task_id_ = get_task_id();
  arg.trace_id_ = get_trace_id();
  arg.tenant_id_ = get_tenant_id();
  arg.incarnation_id_ = incarnation_id_;
  arg.backup_set_id_ = backup_set_id_;
  arg.backup_date_ = backup_date_;
  arg.backup_type_ = backup_type_.type_;
  arg.turn_id_ = turn_id_;
  arg.retry_id_ = retry_id_;
  arg.dst_server_ = get_dst();
  arg.start_turn_id_ = start_turn_id_;
  if (OB_FAIL(backup_status_.get_backup_data_type(arg.backup_data_type_))) {
    LOG_WARN("failed to get backup data type", K(ret), K_(backup_status));
  } else if (OB_FAIL(arg.backup_path_.assign(backup_path_))) {
    LOG_WARN("failed to assign backup path", K(ret), K(backup_path_));
  } else if (OB_FAIL(rpc_proxy.to(get_dst()).backup_build_index(arg))) {
    LOG_WARN("fail to send backup ls data task", K(ret), K(arg));
  } else {
    FLOG_INFO("start to backup build index", K(arg));
  }
  return ret;
}

int ObBackupBuildIndexTask::cancel(obrpc::ObSrvRpcProxy &rpc_proxy) const
{
  int ret = OB_SUCCESS;
  obrpc::ObCancelTaskArg rpc_arg;
  rpc_arg.task_id_ = get_trace_id();
  if (OB_FAIL(rpc_proxy.to(get_dst()).cancel_sys_task(rpc_arg))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      LOG_INFO("task may not excute on server", K(rpc_arg), "dst", get_dst());
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("failed to cancel sys task", K(ret), K(rpc_arg));
    }
  }
  return ret;
}

int ObBackupBuildIndexTask::build(const ObBackupJobAttr &job_attr, const ObBackupSetTaskAttr &set_task_attr, const ObBackupLSTaskAttr &ls_attr)
{
  int ret = OB_SUCCESS;
  ObBackupScheduleTaskKey key;
  if (!job_attr.is_valid() || !ls_attr.is_valid()) {
    ret = OB_SUCCESS;
    LOG_WARN("invalid argument", K(ret), K(job_attr), K(ls_attr));
  } else if (OB_FAIL(key.init(ls_attr.tenant_id_, job_attr.job_id_, ls_attr.task_id_, ls_attr.ls_id_.id(), BackupJobType::BACKUP_DATA_JOB))) {
    LOG_WARN("failed to init backup schedule task key", K(ret), K(job_attr), K(ls_attr));
  } else if (OB_FAIL(ObBackupScheduleTask::build(key, ls_attr.task_trace_id_, ls_attr.status_, ls_attr.dst_))) {
    LOG_WARN("fail to build backup schedule task", K(ret), "trace_id", ls_attr.task_trace_id_, "status", ls_attr.status_, "dst", ls_attr.dst_);
  } else {
    incarnation_id_ = job_attr.incarnation_id_;
    backup_set_id_ = ls_attr.backup_set_id_;
    backup_type_.type_ = ls_attr.backup_type_.type_;
    backup_date_ = ls_attr.backup_date_;
    turn_id_ = ls_attr.turn_id_;
    retry_id_ = ls_attr.retry_id_;
    start_turn_id_ = ls_attr.start_turn_id_;
    backup_status_.status_ = set_task_attr.status_.status_;
    if (OB_FAIL(backup_path_.assign(job_attr.backup_path_))) {
      LOG_WARN("failed to assign backup path", K(ret), "backup_path", job_attr.backup_path_);
    } else if (OB_FAIL(set_optional_servers_(ls_attr.black_servers_))) {
      LOG_WARN("failed to set optional servers", K(ret), K(ls_attr));
    }
  }
  return ret;
}

int ObBackupBuildIndexTask::set_optional_servers_(const ObIArray<common::ObAddr> &black_servers)
{
  int ret = OB_SUCCESS;
  ObLSInfo ls_info;
  ObArray<ObBackupServer> servers;
  uint64_t tenant_id = get_tenant_id();
  share::ObLSTableOperator *lst_operator = GCTX.lst_operator_;
  int64_t cluster_id = GCONF.cluster_id;
  share::ObLSID ls_id(share::ObLSID::SYS_LS_ID);
  if (nullptr == lst_operator) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("lst_operator ptr is null", K(ret));
  } else if (OB_FAIL(lst_operator->get(cluster_id, tenant_id, ls_id, ls_info))) {
    LOG_WARN("failed to get log stream info", K(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else {
    const ObLSInfo::ReplicaArray &replica_array = ls_info.get_replicas();
    for (int i = 0; OB_SUCC(ret) && i < replica_array.count(); ++i) {
      const ObLSReplica &replica = replica_array.at(i);
      if (replica.is_in_service() && !replica.is_strong_leader() && replica.is_valid() 
          && !check_replica_in_black_server_(replica, black_servers)) { 
        ObBackupServer server;
        server.set(replica.get_server(), 0/*high priority*/);
        if (OB_FAIL(servers.push_back(server))) {
          LOG_WARN("failed to push server", K(ret), K(server));
        }
      }
    }
    for (int i = 0; OB_SUCC(ret) && i < replica_array.count(); ++i) {
      const ObLSReplica &replica = replica_array.at(i);
      if (replica.is_in_service() && replica.is_strong_leader() && replica.is_valid()
          && (replica_array.count() == 1 || !check_replica_in_black_server_(replica, black_servers))) {
        // if only has one replica. no use black server.
        ObBackupServer server;
        server.set(replica.get_server(), 1/*low priority*/);
        if (OB_FAIL(servers.push_back(server))) {
          LOG_WARN("failed to push server", K(ret), K(server));
        }
      }
    }
    if (OB_SUCC(ret) && servers.empty()) {
      ret = OB_EAGAIN;
      LOG_WARN("no optional servers, retry_later", K(ret), K(*this));
    }
  }

  if (OB_SUCC(ret) && OB_FAIL(set_optional_servers(servers))) {
    LOG_WARN("failed to optional servers", K(ret));
  } else {
    FLOG_INFO("task optional servers are：", K(*this), K(servers));
  }
  return ret;
}

bool ObBackupBuildIndexTask::check_replica_in_black_server_(
     const ObLSReplica &replica, const ObIArray<common::ObAddr> &black_servers)
{
  bool is_in_black_servers = false;
  for (int64_t i = 0; i < black_servers.count(); ++i) {
    const ObAddr &server = black_servers.at(i);
    if (server == replica.get_server()) {
      is_in_black_servers = true;
      break;
    }
  }
  return is_in_black_servers;
}

/*
 *-------------------------ObBackupCleanLSTask------------------------------
 */

ObBackupCleanLSTask::ObBackupCleanLSTask()
  : job_id_(OB_BACKUP_INVALID_JOB_ID),
    incarnation_id_(OB_BACKUP_INVALID_INCARNATION_ID),
    id_(OB_BACKUP_INVALID_BACKUP_SET_ID),
    round_id_(OB_ARCHIVE_INVALID_ROUND_ID),
    task_type_(),
    ls_id_(),
    dest_id_(OB_INVALID_DEST_ID),
    backup_path_()
{
}

ObBackupCleanLSTask::~ObBackupCleanLSTask()
{
}

int ObBackupCleanLSTask::clone(void *input_ptr, ObBackupScheduleTask *&out_task) const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(input_ptr)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(input_ptr));
  } else {
    ObBackupCleanLSTask *ls_task = new (input_ptr) ObBackupCleanLSTask();
    if (OB_ISNULL(ls_task)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("taks is nullptr", K(ret));
    } else if (OB_FAIL(ls_task->ObBackupScheduleTask::deep_copy(*this))) {
      LOG_WARN("fail to deep copy base task", K(ret));
    } else if (OB_FAIL(ls_task->backup_path_.assign(backup_path_))) {
      LOG_WARN("failed to assign backup dest", K(ret));
    } else {
      ls_task->job_id_ = job_id_;
      ls_task->incarnation_id_ = incarnation_id_;
      ls_task->id_ = id_;
      ls_task->round_id_ = round_id_;  
      ls_task->task_type_ = task_type_;
      ls_task->ls_id_ = ls_id_;
      ls_task->dest_id_ = dest_id_;
    }
    if (OB_SUCC(ret)) {
      out_task = ls_task;
    } else if (OB_NOT_NULL(ls_task)) {
      ls_task->~ObBackupCleanLSTask();
      ls_task = nullptr;
    }
  }
  return ret;
}

int64_t ObBackupCleanLSTask::get_deep_copy_size() const
{
  return sizeof(ObBackupCleanLSTask);
}

bool ObBackupCleanLSTask::can_execute_on_any_server() const
{
  return false;
}

int ObBackupCleanLSTask::do_update_dst_and_doing_status_(common::ObMySQLProxy &sql_proxy, common::ObAddr &dst, share::ObTaskId &trace_id) 
{
  int ret = OB_SUCCESS;
  ObLSID ls_id(get_ls_id());
  if (OB_FAIL(ObBackupCleanLSTaskOperator::update_dst_and_status(sql_proxy, get_task_id(), get_tenant_id(), ls_id, trace_id, dst))) {
    LOG_WARN("failed to update task status", K(ret), K(*this), K(dst));
  }
  return ret;
}


int ObBackupCleanLSTask::set_optional_servers_()
{
  int ret = OB_SUCCESS;
  ObArray<ObBackupServer> servers;
  uint64_t tenant_id = get_tenant_id();
  share::ObLocationService *location_service = GCTX.location_service_;
  int64_t cluster_id = GCONF.cluster_id;
  share::ObLSID ls_id(share::ObLSID::SYS_LS_ID);
  share::ObLSLocation location;
  bool is_cache_hit = false;

  if (OB_ISNULL(location_service)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("location_service ptr is null", K(ret));
  } else if (OB_FAIL(location_service->get(cluster_id, tenant_id, ls_id, INT64_MAX/*expire_renew_time*/,
      is_cache_hit, location))) {
    LOG_WARN("failed to get location", K(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else {
    const common::ObIArray<ObLSReplicaLocation> &replica_array = location.get_replica_locations();
    for (int i = 0; OB_SUCC(ret) && i < replica_array.count(); ++i) {
      const ObLSReplicaLocation &replica = replica_array.at(i);
      if (replica.is_valid()) { 
        ObBackupServer server;
        server.set(replica.get_server(), 1/*high priority*/);
        if (OB_FAIL(servers.push_back(server))) {
          LOG_WARN("failed to push server", K(ret), K(server));
        }
      }
    }
    if (OB_SUCC(ret) && servers.empty()) {
      ret = OB_EAGAIN;
      LOG_WARN("no optional servers, retry_later", K(ret), K(*this));
    }
  }

  if (OB_SUCC(ret) && OB_FAIL(set_optional_servers(servers))) {
    LOG_WARN("failed to optional servers", K(ret));
  } else {
    FLOG_INFO("task optional servers are：", K(*this), K(servers));
  }
  return ret;
}

int ObBackupCleanLSTask::execute(obrpc::ObSrvRpcProxy &rpc_proxy) const
{
  int ret = OB_SUCCESS;

  obrpc::ObLSBackupCleanArg arg;
  arg.trace_id_ = get_trace_id();
  arg.job_id_ = job_id_;
  arg.tenant_id_ = get_tenant_id();
  arg.incarnation_ = incarnation_id_;
  arg.task_id_ = get_task_id();
  arg.id_ = id_;
  arg.round_id_ = round_id_;
  arg.task_type_ = task_type_;
  arg.ls_id_ = ls_id_;
  arg.dest_id_ = dest_id_;
  if (OB_FAIL(rpc_proxy.to(get_dst()).delete_backup_ls_task(arg))) {
    LOG_WARN("fail to send backup clean ls task", K(ret), K(arg));
  } else {
    FLOG_INFO("start to backup clean ls task", K(arg));
  }

  return ret;
}

int ObBackupCleanLSTask::cancel(obrpc::ObSrvRpcProxy &rpc_proxy) const
{
  int ret = OB_SUCCESS;

  obrpc::ObCancelTaskArg rpc_arg;
  rpc_arg.task_id_ = get_trace_id();
  if (OB_FAIL(rpc_proxy.to(get_dst()).cancel_sys_task(rpc_arg))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      LOG_INFO("task may not excute on server", K(rpc_arg), "dst", get_dst());
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("failed to cancel sys task", K(ret), K(rpc_arg));
    }
  }

  return ret;
}

int ObBackupCleanLSTask::build(const ObBackupCleanTaskAttr &task_attr, const ObBackupCleanLSTaskAttr &ls_attr)
{
  int ret = OB_SUCCESS;
  ObBackupScheduleTaskKey key;
  if (!task_attr.is_valid() || !ls_attr.is_valid()) {
    ret = OB_SUCCESS;
    LOG_WARN("invalid argument", K(ret), K(task_attr), K(ls_attr));
  } else if (OB_FAIL(key.init(ls_attr.tenant_id_, ls_attr.job_id_, ls_attr.task_id_, ls_attr.ls_id_.id(), BackupJobType::BACKUP_CLEAN_JOB))) {
    LOG_WARN("failed to init backup schedule task key", K(ret), K(task_attr), K(ls_attr));
  } else if (OB_FAIL(ObBackupScheduleTask::build(key, ls_attr.task_trace_id_, ls_attr.status_, ls_attr.dst_))) {
    LOG_WARN("fail to build backup schedule task", K(ret), "trace_id", ls_attr.task_trace_id_, "status", ls_attr.status_, "dst", ls_attr.dst_);
  } else {
    job_id_ = task_attr.job_id_;
    incarnation_id_ = task_attr.incarnation_id_; 
    round_id_ = ls_attr.round_id_;  
    task_type_ = ls_attr.task_type_;
    ls_id_ = ls_attr.ls_id_;
    dest_id_ = task_attr.dest_id_;
    if (OB_FAIL(backup_path_.assign(task_attr.backup_path_))) {
      LOG_WARN("failed to assign backup path", K(ret), K(task_attr.backup_path_));
    } else if (OB_FAIL(task_attr.get_backup_clean_id(id_))) {
      LOG_WARN("failed to get task id", K(ret), K(task_attr)); 
    } else if (OB_FAIL(set_optional_servers_())) {
      LOG_WARN("failed to set optional servers", K(ret), K(task_attr));  
    }
  }
  return ret;
}

/*
 *-------------------------------ObBackupDataLSMetaTask---------------------------------
 */

int ObBackupDataLSMetaTask::clone(void *input_ptr, ObBackupScheduleTask *&out_task) const 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(input_ptr)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(input_ptr));
  } else {
    ObBackupDataLSMetaTask *my_task = new (input_ptr) ObBackupDataLSMetaTask();
    if (OB_ISNULL(my_task)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("taks is nullptr", K(ret));
    } else if (OB_FAIL(my_task->ObBackupScheduleTask::deep_copy(*this))) {
      LOG_WARN("fail to deep copy base task", K(ret));
    } else if (OB_FAIL(my_task->backup_path_.assign(backup_path_))) {
      LOG_WARN("failed to assign backup path", K(ret));
    } else {
      my_task->incarnation_id_ = incarnation_id_;
      my_task->backup_set_id_ = backup_set_id_;
      my_task->backup_type_.type_ = backup_type_.type_;
      my_task->backup_date_ = backup_date_;
      my_task->ls_id_ = ls_id_;
      my_task->turn_id_ = turn_id_;
      my_task->retry_id_ = retry_id_;
      my_task->start_scn_ = start_scn_;
    }
    if (OB_SUCC(ret)) {
      out_task = my_task;
    } else if (OB_NOT_NULL(my_task)) {
      my_task->~ObBackupDataLSMetaTask();
      my_task = nullptr;
    }
  }
  return ret;
}

int64_t ObBackupDataLSMetaTask::get_deep_copy_size() const
{
  return sizeof(ObBackupDataLSMetaTask);
}

int ObBackupDataLSMetaTask::execute(obrpc::ObSrvRpcProxy &rpc_proxy) const
{
  int ret = OB_SUCCESS;
  obrpc::ObBackupMetaArg arg;
  arg.trace_id_ = get_trace_id();
  arg.tenant_id_ = get_tenant_id();
  arg.task_id_ = get_task_id();
  arg.backup_set_id_ = backup_set_id_;
  arg.incarnation_id_ = incarnation_id_;
  arg.backup_type_ = backup_type_.type_;
  arg.backup_date_ = backup_date_;
  arg.ls_id_ = ls_id_;
  arg.turn_id_ = turn_id_;
  arg.retry_id_ = retry_id_;
  arg.dst_server_ = get_dst();
  arg.job_id_ = get_job_id();
  arg.start_scn_ = start_scn_;
  if (OB_FAIL(arg.backup_path_.assign(backup_path_))) {
    LOG_WARN("failed to assign backup path", K(ret), K(backup_path_));
  } else if (OB_FAIL(rpc_proxy.to(get_dst()).backup_meta(arg))) {
    LOG_WARN("fail to send backup meta task", K(ret), K(arg));
  } else {
    LOG_INFO("start to backup meta", K(arg));
  }
  return ret;
}

}  // namespace rootserver
}  // namespace oceanbase
