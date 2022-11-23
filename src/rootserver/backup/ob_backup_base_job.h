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

#ifndef OCEANBASE_ROOTSERVER_OB_BACKUP_BASE_JOB_H_
#define OCEANBASE_ROOTSERVER_OB_BACKUP_BASE_JOB_H_

#include "ob_backup_task_scheduler.h"
namespace oceanbase 
{
namespace rootserver 
{

class ObIBackupJobScheduler 
{
public:
  ObIBackupJobScheduler(const BackupJobType type) : job_type_(type) {}
  virtual ~ObIBackupJobScheduler() {}
  // job status move forward、generate task、and add task
  virtual int process() = 0;
  virtual int force_cancel(const uint64_t &tenant_id) = 0;    
  // if can_remove return true, scheudler can remove task from scheduler  
  virtual int handle_execute_over(const ObBackupScheduleTask *task, bool &can_remove, 
                                  const ObAddr &black_server, const int execute_ret) = 0;
  virtual int get_need_reload_task(common::ObIAllocator &allocator, 
                                   common::ObIArray<ObBackupScheduleTask *> &tasks) = 0; // reload tasks after switch master happend
public:
  BackupJobType get_job_type() const { return job_type_; }
  TO_STRING_KV(K_(job_type));
protected:
  BackupJobType job_type_;
};

enum class BackupTriggerType : int64_t
{
  BACKUP_AUTO_DELETE_TRIGGER = 0,
  MAX_TRIGGER
};

class ObIBackupTrigger
{
public:
  ObIBackupTrigger(const BackupTriggerType type) : trigger_type_(type) {}
  virtual ~ObIBackupTrigger() {}
  virtual int process() = 0;
public:
  BackupTriggerType get_trigger_type() const { return trigger_type_; }
  TO_STRING_KV(K_(trigger_type));
protected:
  BackupTriggerType trigger_type_;
};
}
}

#endif  