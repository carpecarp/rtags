#include "IndexerSyncer.h"
#include "LevelDB.h"

IndexerSyncer::IndexerSyncer(QObject* parent)
    : QThread(parent), mStopped(false)
{
}

void IndexerSyncer::stop()
{
    QMutexLocker locker(&mMutex);
    mStopped = true;
    mCond.wakeOne();
}

void IndexerSyncer::notify()
{
    QMutexLocker locker(&mMutex); // is this needed here?
    mCond.wakeOne();
}

void IndexerSyncer::addSymbolNames(const SymbolNameHash &locations)
{
    QMutexLocker lock(&mMutex);
    if (mSymbolNames.isEmpty()) {
        mSymbolNames = locations;
    } else {
        const SymbolNameHash::const_iterator end = locations.end();
        for (SymbolNameHash::const_iterator it = locations.begin(); it != end; ++it) {
            mSymbolNames[it.key()].unite(it.value());
        }
    }
}

void IndexerSyncer::addSymbols(const SymbolHash &symbols)
{
    QMutexLocker lock(&mMutex);
    if (mSymbols.isEmpty()) {
        mSymbols = symbols;
    } else {
        const SymbolHash::const_iterator end = symbols.end();
        for (SymbolHash::const_iterator it = symbols.begin(); it != end; ++it) {
            mSymbols[it.key()].unite(it.value());
        }
    }
}

void IndexerSyncer::addReferences(const ReferenceHash &references)
{
    QMutexLocker lock(&mMutex);
    if (mReferences.isEmpty()) {
        mReferences = references;
    } else {
        const ReferenceHash::const_iterator end = references.end();
        for (ReferenceHash::const_iterator it = references.begin(); it != end; ++it) {
            mReferences[it.key()] = it.value();
        }
    }
}


void IndexerSyncer::addDependencies(const DependencyHash& dependencies)
{
    QMutexLocker lock(&mMutex);
    if (mDependencies.isEmpty()) {
        mDependencies = dependencies;
    } else {
        const DependencyHash::const_iterator end = dependencies.end();
        for (DependencyHash::const_iterator it = dependencies.begin(); it != end; ++it) {
            mDependencies[it.key()].unite(it.value());
        }
    }
}

void IndexerSyncer::setPchDependencies(const DependencyHash& dependencies)
{
    QMutexLocker lock(&mMutex);
    if (mPchDependencies.isEmpty()) {
        mPchDependencies = dependencies;
    } else {
        const DependencyHash::const_iterator end = dependencies.end();
        for (DependencyHash::const_iterator it = dependencies.begin(); it != end; ++it) {
            mPchDependencies[it.key()].unite(it.value());
        }
    }
}


void IndexerSyncer::addFileInformation(const Path& input, const QList<QByteArray>& args, time_t timeStamp)
{
    FileInformation fi;
    fi.lastTouched = timeStamp;
    fi.compileArgs = args;
    QMutexLocker lock(&mMutex);
    mInformations[input] = fi;
}

template <typename Container, typename Value>
static inline bool addTo(Container &container, const Value &value)
{
    const int oldSize = container.size();
    container += value;
    return container.size() != oldSize;
}

void IndexerSyncer::run()
{
    while (true) {
        SymbolNameHash symbolNames;
        SymbolHash symbols;
        DependencyHash dependencies, pchDependencies;
        InformationHash informations;
        ReferenceHash references;
        {
            QMutexLocker locker(&mMutex);
            if (mStopped)
                return;
            while (mSymbols.isEmpty()
                   && mSymbolNames.isEmpty()
                   && mDependencies.isEmpty()
                   && mInformations.isEmpty()
                   && mReferences.isEmpty()
                   && mPchDependencies.isEmpty()) {
                mCond.wait(&mMutex, 10000);
                if (mStopped)
                    return;

            }
            qSwap(symbolNames, mSymbolNames);
            qSwap(symbols, mSymbols);
            qSwap(dependencies, mDependencies);
            qSwap(pchDependencies, mPchDependencies);
            qSwap(informations, mInformations);
            qSwap(references, mReferences);
        }
        if (!symbolNames.isEmpty()) {
            LevelDB db;
            if (!db.open(Database::SymbolName, LevelDB::ReadWrite))
                return;

            leveldb::WriteBatch batch;

            SymbolNameHash::iterator it = symbolNames.begin();
            const SymbolNameHash::const_iterator end = symbolNames.end();
            bool changed = false;
            while (it != end) {
                const char *key = it.key().constData();
                const QSet<RTags::Location> added = it.value();
                QSet<RTags::Location> current = Rdm::readValue<QSet<RTags::Location> >(db.db(), key);
                if (addTo(current, added)) {
                    changed = true;
                    Rdm::writeValue<QSet<RTags::Location> >(&batch, key, current);
                }
                ++it;
            }

            if (changed)
                db.db()->Write(leveldb::WriteOptions(), &batch);
        }
        LevelDB symbolDB;
        bool changedSymbols = false;
        leveldb::WriteBatch symbolsBatch;

        if (!references.isEmpty()) {
            const ReferenceHash::const_iterator end = references.end();
            for (ReferenceHash::const_iterator it = references.begin(); it != end; ++it) {
                SymbolHash::iterator sym = symbols.find(it.value().first);
                if (sym != symbols.end()) {
                    Rdm::CursorInfo &ci = sym.value();
                    ci.references.insert(it.key());
                    // if (it.value().first.path.contains("RTags.h"))
                    //     error() << "cramming" << it.key() << "into" << it.value();
                    if (it.value().second != Rdm::NormalReference) {
                        Rdm::CursorInfo &other = symbols[it.key()];
                        ci.references += other.references;
                        other.references += ci.references;
                        if (other.target.isNull()) {
                            // if (it.key().key().contains("RTags."))
                            error() << "1looking up" << it.key() << "with the intention of setting target to" << it.value().first
                                    << "it was" << other.target;
                        }
                        
                        if (other.target.isNull())
                            other.target = it.value().first;
                    }
                } else {
                    if (!symbolDB.db()) {
                        QByteArray err;
                        if (!symbolDB.open(Database::Symbol, LevelDB::ReadWrite, &err)) {
                            error("Can't open Symbol database %s %s\n",
                                  Database::databaseName(Database::Symbol).constData(),
                                  err.constData());
                            return;
                        }
                    }
                    const QByteArray key = it.value().first.key(RTags::Location::Padded);
                    Rdm::CursorInfo current = Rdm::readValue<Rdm::CursorInfo>(symbolDB.db(), key.constData());
                    bool changedCurrent = false;
                    if (addTo(current.references, it.key()))
                        changedCurrent = true;
                    if (it.value().second != Rdm::NormalReference) {
                        const QByteArray otherKey = it.key().key(RTags::Location::Padded);
                        Rdm::CursorInfo other = Rdm::readValue<Rdm::CursorInfo>(symbolDB.db(), otherKey);
                        bool changedOther = false;
                        if (addTo(other.references, it.key()))
                            changedOther = true;
                        if (addTo(other.references, current.references))
                            changedOther = true;
                        if (addTo(current.references, other.references))
                            changedCurrent = true;

                        if (other.target.isNull()) {
                            // if (otherKey.contains("RTags."))
                            error() << "2looking up" << otherKey << "with the intention of setting target to" << it.value().first
                                    << "it was" << other.target;
                        }
                        
                            
                        if (other.target.isNull()) {
                            other.target = it.value().first;
                            changedOther = true;
                        }
                        if (changedOther) {
                            changedSymbols = true;
                            Rdm::writeValue<Rdm::CursorInfo>(&symbolsBatch, otherKey, other);
                        }
                        // error() << "ditched reference" << it.key() << it.value();
                    }
                    if (changedCurrent) {
                        changedSymbols = true;
                        Rdm::writeValue<Rdm::CursorInfo>(&symbolsBatch, key, current);
                    }
                }
            }
        }
        if (!symbols.isEmpty()) {
            QByteArray err;
            if (!symbolDB.db() && !symbolDB.open(Database::Symbol, LevelDB::ReadWrite, &err)) {
                error("Can't open Symbol database %s %s\n",
                      Database::databaseName(Database::Symbol).constData(),
                      err.constData());
                return;
            }

            SymbolHash::iterator it = symbols.begin();
            const SymbolHash::const_iterator end = symbols.end();
            while (it != end) {
                const QByteArray key = it.key().key(RTags::Location::Padded);
                Rdm::CursorInfo added = it.value();
                Rdm::CursorInfo current = Rdm::readValue<Rdm::CursorInfo>(symbolDB.db(), key.constData());
                if (current.unite(added)) {
                    changedSymbols = true;
                    Rdm::writeValue<Rdm::CursorInfo>(&symbolsBatch, key, current);
                }
                ++it;
            }
        }
        if (changedSymbols)
            symbolDB.db()->Write(leveldb::WriteOptions(), &symbolsBatch);
        symbolDB.close();
        
        if (!dependencies.isEmpty()) {
            LevelDB db;
            if (!db.open(Database::Dependency, LevelDB::ReadWrite))
                return;
            leveldb::WriteBatch batch;

            DependencyHash::iterator it = dependencies.begin();
            const DependencyHash::const_iterator end = dependencies.end();
            bool changed = false;
            while (it != end) {
                const char* key = it.key().constData();
                QSet<Path> added = it.value();
                QSet<Path> current = Rdm::readValue<QSet<Path> >(db.db(), key);
                const int oldSize = current.size();
                if (current.unite(added).size() > oldSize) {
                    changed = true;
                    Rdm::writeValue<QSet<Path> >(&batch, key, current);
                }
                ++it;
            }

            if (changed)
                db.db()->Write(leveldb::WriteOptions(), &batch);
        }
        if (!pchDependencies.isEmpty()) {
            LevelDB db;
            if (!db.open(Database::Dependency, LevelDB::ReadWrite))
                return;
            Rdm::writeValue<DependencyHash>(db.db(), "pch", pchDependencies);
        }
        if (!informations.isEmpty()) {
            leveldb::WriteBatch batch;

            InformationHash::iterator it = informations.begin();
            const InformationHash::const_iterator end = informations.end();
            while (it != end) {
                const char *key = it.key().constData();
                Rdm::writeValue<FileInformation>(&batch, key, it.value());
                ++it;
            }
            LevelDB db;
            if (!db.open(Database::FileInformation, LevelDB::ReadWrite))
                return;

            db.db()->Write(leveldb::WriteOptions(), &batch);
        }
    }
}
