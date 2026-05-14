#include "shutdowncoordinator.h"
#include <QtGlobal>

ShutdownCoordinator::ShutdownCoordinator(TimeTracker& timetracker, IDatabaseManager& db, const Settings& settings)
    : timetracker_(timetracker), db_(db), settings_(settings), shutdown_completed_(false)
{
}

void ShutdownCoordinator::run(bool forceDirectPath)
{
    Q_UNUSED(forceDirectPath)
    Q_ASSERT_X(false, "ShutdownCoordinator::run", "stub — not yet implemented");
}
