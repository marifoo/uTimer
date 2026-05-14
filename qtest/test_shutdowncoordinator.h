#ifndef TEST_SHUTDOWNCOORDINATOR_H
#define TEST_SHUTDOWNCOORDINATOR_H

#include <QObject>
#include "testcommon.h"

class ShutdownCoordinatorTest : public QObject
{
    Q_OBJECT

private slots:
    // Test G: happy path — stop, flush, marker
    void test_G_happy_path_stop_flush_marker();

    // Test H: idempotent — second run() is a no-op
    void test_H_idempotent_second_run_is_noop();

    // Test I: force-direct path skips the retry loop
    void test_I_force_direct_skips_retry_loop();
};

#endif // TEST_SHUTDOWNCOORDINATOR_H
