#ifndef SHUTDOWNCOORDINATOR_H
#define SHUTDOWNCOORDINATOR_H

class Timer;
class SessionStore;

class ShutdownCoordinator
{
public:
    ShutdownCoordinator(Timer& timetracker, SessionStore& db);

    void run(bool forceDirectPath = false);

private:
    void pumpEvents(int budgetMs);

    Timer& timetracker_;
    SessionStore& db_;
    bool shutdown_completed_;
};

#endif // SHUTDOWNCOORDINATOR_H
