#ifndef ClangRunnable_h
#define ClangRunnable_h

#include <QtCore>
#include <clang-c/Index.h>
#include "Path.h"
#include "GccArguments.h"

class ParseThread : public QThread
{
    Q_OBJECT
public:
    ParseThread();
    ~ParseThread();
    void abort();
    void load(const Path &path);
    void reparse(const Path &path);
    void handleFileChanged(const Path &path);
signals:
    void invalidated(const Path &path);
    void fileParsed(const Path &path, void *translationUnit);
    void parseError(const Path &path);
protected:
    void run();
private:
    QMutex mMutex;
    QWaitCondition mWaitCondition;
    bool mAborted;
    struct File {
        Path path;
        GccArguments arguments;
        QObject *receiver;
        const char *member;
        File *next;
    } *mFirst, *mLast;
    int mCount;
    QHash<Path, QSet<Path> > mDependencies;
    CXIndex mIndex;
};

#endif
