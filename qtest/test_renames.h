#ifndef TEST_RENAMES_H
#define TEST_RENAMES_H

#include <QObject>

class RenamesTest : public QObject
{
    Q_OBJECT

private slots:
    void test_no_old_names_in_production_headers();
};

#endif // TEST_RENAMES_H
