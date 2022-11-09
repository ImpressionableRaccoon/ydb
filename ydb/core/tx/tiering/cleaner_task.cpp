#include "cleaner_task.h"
#include "path_cleaner.h"

namespace NKikimr::NColumnShard::NTiers {

TTaskCleanerActivity::TFactory::TRegistrator<TTaskCleanerActivity> TTaskCleanerActivity::Registrator(TTaskCleanerActivity::GetClassNameStatic());

NKikimrSchemeOp::TTaskCleaner TTaskCleanerActivity::DoSerializeToProto() const {
    NKikimrSchemeOp::TTaskCleaner result;
    result.SetPathId(PathId);
    result.SetOwnerPath(OwnerPath);
    return result;
}

bool TTaskCleanerActivity::DoDeserializeFromProto(const NKikimrSchemeOp::TTaskCleaner& protoData) {
    PathId = protoData.GetPathId();
    OwnerPath = protoData.GetOwnerPath();
    return true;
}

void TTaskCleanerActivity::DoExecute(NBackgroundTasks::ITaskExecutorController::TPtr controller,
    const NBackgroundTasks::TTaskStateContainer& /*state*/)
{
#ifndef KIKIMR_DISABLE_S3_OPS
    TActivationContext::AsActorContext().Register(new TPathCleaner(PathId, OwnerPath, controller));
#else
    controller->TaskFinished();
#endif
}

}
