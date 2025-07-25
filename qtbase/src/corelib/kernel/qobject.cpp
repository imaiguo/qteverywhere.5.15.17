/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Copyright (C) 2016 Intel Corporation.
** Copyright (C) 2013 Olivier Goffart <ogoffart@woboq.com>
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qobject.h"
#include "qobject_p.h"
#include "qmetaobject_p.h"

#include "qabstracteventdispatcher.h"
#include "qabstracteventdispatcher_p.h"
#include "qcoreapplication.h"
#include "qcoreapplication_p.h"
#include "qloggingcategory.h"
#include "qvariant.h"
#include "qmetaobject.h"
#include <qregexp.h>
#if QT_CONFIG(regularexpression)
#  include <qregularexpression.h>
#endif
#include <qthread.h>
#include <private/qthread_p.h>
#include <qdebug.h>
#include <qpair.h>
#include <qvarlengtharray.h>
#include <qscopeguard.h>
#include <qset.h>
#if QT_CONFIG(thread)
#include <qsemaphore.h>
#endif
#include <qsharedpointer.h>

#include <private/qorderedmutexlocker_p.h>
#include <private/qhooks_p.h>
#include <qtcore_tracepoints_p.h>

#include <new>
#include <mutex>

#include <ctype.h>
#include <limits.h>

QT_BEGIN_NAMESPACE

static int DIRECT_CONNECTION_ONLY = 0;

Q_LOGGING_CATEGORY(lcConnections, "qt.core.qmetaobject.connectslotsbyname")

Q_CORE_EXPORT QBasicAtomicPointer<QSignalSpyCallbackSet> qt_signal_spy_callback_set = Q_BASIC_ATOMIC_INITIALIZER(nullptr);

void qt_register_signal_spy_callbacks(QSignalSpyCallbackSet *callback_set)
{
    qt_signal_spy_callback_set.storeRelease(callback_set);
}

QDynamicMetaObjectData::~QDynamicMetaObjectData()
{
}

QAbstractDynamicMetaObject::~QAbstractDynamicMetaObject()
{
}

static int *queuedConnectionTypes(const QList<QByteArray> &typeNames)
{
    int *types = new int [typeNames.count() + 1];
    Q_CHECK_PTR(types);
    for (int i = 0; i < typeNames.count(); ++i) {
        const QByteArray typeName = typeNames.at(i);
        if (typeName.endsWith('*'))
            types[i] = QMetaType::VoidStar;
        else
            types[i] = QMetaType::type(typeName);

        if (!types[i]) {
            qWarning("QObject::connect: Cannot queue arguments of type '%s'\n"
                     "(Make sure '%s' is registered using qRegisterMetaType().)",
                     typeName.constData(), typeName.constData());
            delete [] types;
            return nullptr;
        }
    }
    types[typeNames.count()] = 0;

    return types;
}

static int *queuedConnectionTypes(const QArgumentType *argumentTypes, int argc)
{
    QScopedArrayPointer<int> types(new int [argc + 1]);
    for (int i = 0; i < argc; ++i) {
        const QArgumentType &type = argumentTypes[i];
        if (type.type())
            types[i] = type.type();
        else if (type.name().endsWith('*'))
            types[i] = QMetaType::VoidStar;
        else
            types[i] = QMetaType::type(type.name());

        if (!types[i]) {
            qWarning("QObject::connect: Cannot queue arguments of type '%s'\n"
                     "(Make sure '%s' is registered using qRegisterMetaType().)",
                     type.name().constData(), type.name().constData());
            return nullptr;
        }
    }
    types[argc] = 0;

    return types.take();
}

static QBasicMutex _q_ObjectMutexPool[131];

/**
 * \internal
 * mutex to be locked when accessing the connection lists or the senders list
 */
static inline QBasicMutex *signalSlotLock(const QObject *o)
{
    return &_q_ObjectMutexPool[uint(quintptr(o)) % sizeof(_q_ObjectMutexPool)/sizeof(QBasicMutex)];
}

#if QT_VERSION < 0x60000
extern "C" Q_CORE_EXPORT void qt_addObject(QObject *)
{}

extern "C" Q_CORE_EXPORT void qt_removeObject(QObject *)
{}
#endif

void (*QAbstractDeclarativeData::destroyed)(QAbstractDeclarativeData *, QObject *) = nullptr;
void (*QAbstractDeclarativeData::destroyed_qml1)(QAbstractDeclarativeData *, QObject *) = nullptr;
void (*QAbstractDeclarativeData::parentChanged)(QAbstractDeclarativeData *, QObject *, QObject *) = nullptr;
void (*QAbstractDeclarativeData::signalEmitted)(QAbstractDeclarativeData *, QObject *, int, void **) = nullptr;
int  (*QAbstractDeclarativeData::receivers)(QAbstractDeclarativeData *, const QObject *, int) = nullptr;
bool (*QAbstractDeclarativeData::isSignalConnected)(QAbstractDeclarativeData *, const QObject *, int) = nullptr;
void (*QAbstractDeclarativeData::setWidgetParent)(QObject *, QObject *) = nullptr;

/*!
    \fn QObjectData::QObjectData()
    \internal
 */


QObjectData::~QObjectData() {}

QMetaObject *QObjectData::dynamicMetaObject() const
{
    return metaObject->toDynamicMetaObject(q_ptr);
}

QObjectPrivate::QObjectPrivate(int version)
    : threadData(nullptr), currentChildBeingDeleted(nullptr)
{
    checkForIncompatibleLibraryVersion(version);

    // QObjectData initialization
    q_ptr = nullptr;
    parent = nullptr;                           // no parent yet. It is set by setParent()
    isWidget = false;                           // assume not a widget object
    blockSig = false;                           // not blocking signals
    wasDeleted = false;                         // double-delete catcher
    isDeletingChildren = false;                 // set by deleteChildren()
    sendChildEvents = true;                     // if we should send ChildAdded and ChildRemoved events to parent
    receiveChildEvents = true;
    postedEvents = 0;
    extraData = nullptr;
    metaObject = nullptr;
    isWindow = false;
    deleteLaterCalled = false;
}

QObjectPrivate::~QObjectPrivate()
{
    auto thisThreadData = threadData.loadRelaxed();
    if (extraData && !extraData->runningTimers.isEmpty()) {
        if (Q_LIKELY(thisThreadData->thread.loadAcquire() == QThread::currentThread())) {
            // unregister pending timers
            if (thisThreadData->hasEventDispatcher())
                thisThreadData->eventDispatcher.loadRelaxed()->unregisterTimers(q_ptr);

            // release the timer ids back to the pool
            for (int i = 0; i < extraData->runningTimers.size(); ++i)
                QAbstractEventDispatcherPrivate::releaseTimerId(extraData->runningTimers.at(i));
        } else {
            qWarning("QObject::~QObject: Timers cannot be stopped from another thread");
        }
    }

    if (postedEvents)
        QCoreApplication::removePostedEvents(q_ptr, 0);

    thisThreadData->deref();

    if (metaObject) metaObject->objectDestroyed(q_ptr);

#ifndef QT_NO_USERDATA
    if (extraData)
        qDeleteAll(extraData->userData);
#endif
    delete extraData;
}

/*!
  \internal
  For a given metaobject, compute the signal offset, and the method offset (including signals)
*/
static void computeOffsets(const QMetaObject *metaobject, int *signalOffset, int *methodOffset)
{
    *signalOffset = *methodOffset = 0;
    const QMetaObject *m = metaobject->d.superdata;
    while (m) {
        const QMetaObjectPrivate *d = QMetaObjectPrivate::get(m);
        *methodOffset += d->methodCount;
        Q_ASSERT(d->revision >= 4);
        *signalOffset += d->signalCount;
        m = m->d.superdata;
    }
}

// Used by QAccessibleWidget
bool QObjectPrivate::isSender(const QObject *receiver, const char *signal) const
{
    Q_Q(const QObject);
    int signal_index = signalIndex(signal);
    ConnectionData *cd = connections.loadRelaxed();
    if (signal_index < 0 || !cd)
        return false;
    QBasicMutexLocker locker(signalSlotLock(q));
    if (signal_index < cd->signalVectorCount()) {
        const QObjectPrivate::Connection *c = cd->signalVector.loadRelaxed()->at(signal_index).first.loadRelaxed();

        while (c) {
            if (c->receiver.loadRelaxed() == receiver)
                return true;
            c = c->nextConnectionList.loadRelaxed();
        }
    }
    return false;
}

// Used by QAccessibleWidget
QObjectList QObjectPrivate::receiverList(const char *signal) const
{
    QObjectList returnValue;
    int signal_index = signalIndex(signal);
    ConnectionData *cd = connections.loadRelaxed();
    if (signal_index < 0 || !cd)
        return returnValue;
    if (signal_index < cd->signalVectorCount()) {
        const QObjectPrivate::Connection *c = cd->signalVector.loadRelaxed()->at(signal_index).first.loadRelaxed();

        while (c) {
            QObject *r = c->receiver.loadRelaxed();
            if (r)
                returnValue << r;
            c = c->nextConnectionList.loadRelaxed();
        }
    }
    return returnValue;
}

// Used by QAccessibleWidget
QObjectList QObjectPrivate::senderList() const
{
    QObjectList returnValue;
    ConnectionData *cd = connections.loadRelaxed();
    if (cd) {
        QBasicMutexLocker locker(signalSlotLock(q_func()));
        for (Connection *c = cd->senders; c; c = c->next)
            returnValue << c->sender;
    }
    return returnValue;
}

/*!
  \internal
  Add the connection \a c to the list of connections of the sender's object
  for the specified \a signal

  The signalSlotLock() of the sender and receiver must be locked while calling
  this function

  Will also add the connection in the sender's list of the receiver.
 */
void QObjectPrivate::addConnection(int signal, Connection *c)
{
    Q_ASSERT(c->sender == q_ptr);
    ensureConnectionData();
    ConnectionData *cd = connections.loadRelaxed();
    cd->resizeSignalVector(signal + 1);

    ConnectionList &connectionList = cd->connectionsForSignal(signal);
    if (connectionList.last.loadRelaxed()) {
        Q_ASSERT(connectionList.last.loadRelaxed()->receiver.loadRelaxed());
        connectionList.last.loadRelaxed()->nextConnectionList.storeRelaxed(c);
    } else {
        connectionList.first.storeRelaxed(c);
    }
    c->id = ++cd->currentConnectionId;
    c->prevConnectionList = connectionList.last.loadRelaxed();
    connectionList.last.storeRelaxed(c);

    QObjectPrivate *rd = QObjectPrivate::get(c->receiver.loadRelaxed());
    rd->ensureConnectionData();

    c->prev = &(rd->connections.loadRelaxed()->senders);
    c->next = *c->prev;
    *c->prev = c;
    if (c->next)
        c->next->prev = &c->next;
}

void QObjectPrivate::ConnectionData::removeConnection(QObjectPrivate::Connection *c)
{
    Q_ASSERT(c->receiver.loadRelaxed());
    ConnectionList &connections = signalVector.loadRelaxed()->at(c->signal_index);
    c->receiver.storeRelaxed(nullptr);
    QThreadData *td = c->receiverThreadData.loadRelaxed();
    if (td)
        td->deref();
    c->receiverThreadData.storeRelaxed(nullptr);

#ifndef QT_NO_DEBUG
    bool found = false;
    for (Connection *cc = connections.first.loadRelaxed(); cc; cc = cc->nextConnectionList.loadRelaxed()) {
        if (cc == c) {
            found = true;
            break;
        }
    }
    Q_ASSERT(found);
#endif

    // remove from the senders linked list
    *c->prev = c->next;
    if (c->next)
        c->next->prev = c->prev;
    c->prev = nullptr;

    if (connections.first.loadRelaxed() == c)
        connections.first.storeRelaxed(c->nextConnectionList.loadRelaxed());
    if (connections.last.loadRelaxed() == c)
        connections.last.storeRelaxed(c->prevConnectionList);
    Q_ASSERT(signalVector.loadRelaxed()->at(c->signal_index).first.loadRelaxed() != c);
    Q_ASSERT(signalVector.loadRelaxed()->at(c->signal_index).last.loadRelaxed() != c);

    // keep c->nextConnectionList intact, as it might still get accessed by activate
    Connection *n = c->nextConnectionList.loadRelaxed();
    if (n)
        n->prevConnectionList = c->prevConnectionList;
    if (c->prevConnectionList)
        c->prevConnectionList->nextConnectionList.storeRelaxed(n);
    c->prevConnectionList = nullptr;

    Q_ASSERT(c != orphaned.loadRelaxed());
    // add c to orphanedConnections
    Connection *o = nullptr;
    /* No ABA issue here: When adding a node, we only care about the list head, it doesn't
     * matter if the tail changes.
     */
    do {
        o = orphaned.loadRelaxed();
        c->nextInOrphanList = o;
    } while (!orphaned.testAndSetRelease(o, c));

#ifndef QT_NO_DEBUG
    found = false;
    for (Connection *cc = connections.first.loadRelaxed(); cc; cc = cc->nextConnectionList.loadRelaxed()) {
        if (cc == c) {
            found = true;
            break;
        }
    }
    Q_ASSERT(!found);
#endif

}

void QObjectPrivate::ConnectionData::cleanOrphanedConnectionsImpl(QObject *sender, LockPolicy lockPolicy)
{
    QBasicMutex *senderMutex = signalSlotLock(sender);
    ConnectionOrSignalVector *c = nullptr;
    {
        std::unique_lock<QBasicMutex> lock(*senderMutex, std::defer_lock_t{});
        if (lockPolicy == NeedToLock)
            lock.lock();
        if (ref.loadAcquire() > 1)
            return;

        // Since ref == 1, no activate() is in process since we locked the mutex. That implies,
        // that nothing can reference the orphaned connection objects anymore and they can
        // be safely deleted
        c = orphaned.fetchAndStoreRelaxed(nullptr);
    }
    if (c) {
        // Deleting c might run arbitrary user code, so we must not hold the lock
        if (lockPolicy == AlreadyLockedAndTemporarilyReleasingLock) {
            senderMutex->unlock();
            deleteOrphaned(c);
            senderMutex->lock();
        } else {
            deleteOrphaned(c);
        }
    }
}

void QObjectPrivate::ConnectionData::deleteOrphaned(QObjectPrivate::ConnectionOrSignalVector *o)
{
    while (o) {
        QObjectPrivate::ConnectionOrSignalVector *next = nullptr;
        if (SignalVector *v = ConnectionOrSignalVector::asSignalVector(o)) {
            next = v->nextInOrphanList;
            free(v);
        } else {
            QObjectPrivate::Connection *c = static_cast<Connection *>(o);
            next = c->nextInOrphanList;
            Q_ASSERT(!c->receiver.loadRelaxed());
            Q_ASSERT(!c->prev);
            c->freeSlotObject();
            c->deref();
        }
        o = next;
    }
}

/*! \internal

  Returns \c true if the signal with index \a signal_index from object \a sender is connected.

  \a signal_index must be the index returned by QObjectPrivate::signalIndex;
*/
bool QObjectPrivate::isSignalConnected(uint signalIndex, bool checkDeclarative) const
{
    if (checkDeclarative && isDeclarativeSignalConnected(signalIndex))
        return true;

    ConnectionData *cd = connections.loadRelaxed();
    if (!cd)
        return false;
    SignalVector *signalVector = cd->signalVector.loadRelaxed();
    if (!signalVector)
        return false;

    if (signalVector->at(-1).first.loadRelaxed())
        return true;

    if (signalIndex < uint(cd->signalVectorCount())) {
        const QObjectPrivate::Connection *c = signalVector->at(signalIndex).first.loadRelaxed();
        while (c) {
            if (c->receiver.loadRelaxed())
                return true;
            c = c->nextConnectionList.loadRelaxed();
        }
    }
    return false;
}

bool QObjectPrivate::maybeSignalConnected(uint signalIndex) const
{
    ConnectionData *cd = connections.loadRelaxed();
    if (!cd)
        return false;
    SignalVector *signalVector = cd->signalVector.loadRelaxed();
    if (!signalVector)
        return false;

    if (signalVector->at(-1).first.loadAcquire())
        return true;

    if (signalIndex < uint(cd->signalVectorCount())) {
        const QObjectPrivate::Connection *c = signalVector->at(signalIndex).first.loadAcquire();
        return c != nullptr;
    }
    return false;
}

/*!
    \internal
 */
QAbstractMetaCallEvent::~QAbstractMetaCallEvent()
{
#if QT_CONFIG(thread)
    if (semaphore_)
        semaphore_->release();
#endif
}

/*!
    \internal
 */
inline void QMetaCallEvent::allocArgs()
{
    if (!d.nargs_)
        return;

    constexpr size_t each = sizeof(void*) + sizeof(int);
    void *const memory = d.nargs_ * each > sizeof(prealloc_) ?
        calloc(d.nargs_, each) : prealloc_;

    Q_CHECK_PTR(memory);
    d.args_ = static_cast<void **>(memory);
}

/*!
    \internal

    Used for blocking queued connections, just passes \a args through without
    allocating any memory.
 */
QMetaCallEvent::QMetaCallEvent(ushort method_offset, ushort method_relative,
                               QObjectPrivate::StaticMetaCallFunction callFunction,
                               const QObject *sender, int signalId,
                               void **args, QSemaphore *semaphore)
    : QAbstractMetaCallEvent(sender, signalId, semaphore),
      d({nullptr, args, callFunction, 0, method_offset, method_relative}),
      prealloc_()
{
}

/*!
    \internal

    Used for blocking queued connections, just passes \a args through without
    allocating any memory.
 */
QMetaCallEvent::QMetaCallEvent(QtPrivate::QSlotObjectBase *slotO,
                               const QObject *sender, int signalId,
                               void **args, QSemaphore *semaphore)
    : QAbstractMetaCallEvent(sender, signalId, semaphore),
      d({slotO, args, nullptr, 0, 0, ushort(-1)}),
      prealloc_()
{
    if (d.slotObj_)
        d.slotObj_->ref();
}

/*!
    \internal

    Allocates memory for \a nargs; code creating an event needs to initialize
    the void* and int arrays by accessing \a args() and \a types(), respectively.
 */
QMetaCallEvent::QMetaCallEvent(ushort method_offset, ushort method_relative,
                               QObjectPrivate::StaticMetaCallFunction callFunction,
                               const QObject *sender, int signalId,
                               int nargs)
    : QAbstractMetaCallEvent(sender, signalId),
      d({nullptr, nullptr, callFunction, nargs, method_offset, method_relative}),
      prealloc_()
{
    allocArgs();
}

/*!
    \internal

    Allocates memory for \a nargs; code creating an event needs to initialize
    the void* and int arrays by accessing \a args() and \a types(), respectively.
 */
QMetaCallEvent::QMetaCallEvent(QtPrivate::QSlotObjectBase *slotO,
                               const QObject *sender, int signalId,
                               int nargs)
    : QAbstractMetaCallEvent(sender, signalId),
      d({slotO, nullptr, nullptr, nargs, 0, ushort(-1)}),
      prealloc_()
{
    if (d.slotObj_)
        d.slotObj_->ref();
    allocArgs();
}

/*!
    \internal
 */
QMetaCallEvent::~QMetaCallEvent()
{
    if (d.nargs_) {
        int *typeIDs = types();
        for (int i = 0; i < d.nargs_; ++i) {
            if (typeIDs[i] && d.args_[i])
                QMetaType::destroy(typeIDs[i], d.args_[i]);
        }
        if (reinterpret_cast<void*>(d.args_) != reinterpret_cast<void*>(prealloc_))
            free(d.args_);
    }
    if (d.slotObj_)
        d.slotObj_->destroyIfLastRef();
}

/*!
    \internal
 */
void QMetaCallEvent::placeMetaCall(QObject *object)
{
    if (d.slotObj_) {
        d.slotObj_->call(object, d.args_);
    } else if (d.callFunction_ && d.method_offset_ <= object->metaObject()->methodOffset()) {
        d.callFunction_(object, QMetaObject::InvokeMetaMethod, d.method_relative_, d.args_);
    } else {
        QMetaObject::metacall(object, QMetaObject::InvokeMetaMethod,
                              d.method_offset_ + d.method_relative_, d.args_);
    }
}

/*!
    \class QSignalBlocker
    \brief Exception-safe wrapper around QObject::blockSignals().
    \since 5.3
    \ingroup objectmodel
    \inmodule QtCore

    \reentrant

    QSignalBlocker can be used wherever you would otherwise use a
    pair of calls to blockSignals(). It blocks signals in its
    constructor and in the destructor it resets the state to what
    it was before the constructor ran.

    \snippet code/src_corelib_kernel_qobject.cpp 53
    is thus equivalent to
    \snippet code/src_corelib_kernel_qobject.cpp 54

    except the code using QSignalBlocker is safe in the face of
    exceptions.

    \sa QMutexLocker, QEventLoopLocker
*/

/*!
    \fn QSignalBlocker::QSignalBlocker(QObject *object)

    Constructor. Calls \a{object}->blockSignals(true).
*/

/*!
    \fn QSignalBlocker::QSignalBlocker(QObject &object)
    \overload

    Calls \a{object}.blockSignals(true).
*/

/*!
    \fn QSignalBlocker::QSignalBlocker(QSignalBlocker &&other)

    Move-constructs a signal blocker from \a other. \a other will have
    a no-op destructor, while responsibility for restoring the
    QObject::signalsBlocked() state is transferred to the new object.
*/

/*!
    \fn QSignalBlocker &QSignalBlocker::operator=(QSignalBlocker &&other)

    Move-assigns this signal blocker from \a other. \a other will have
    a no-op destructor, while responsibility for restoring the
    QObject::signalsBlocked() state is transferred to this object.

    The object's signals this signal blocker was blocking prior to
    being moved to, if any, are unblocked \e except in the case where
    both instances block the same object's signals and \c *this is
    unblocked while \a other is not, at the time of the move.
*/

/*!
    \fn QSignalBlocker::~QSignalBlocker()

    Destructor. Restores the QObject::signalsBlocked() state to what it
    was before the constructor ran, unless unblock() has been called
    without a following reblock(), in which case it does nothing.
*/

/*!
    \fn void QSignalBlocker::reblock()

    Re-blocks signals after a previous unblock().

    The numbers of reblock() and unblock() calls are not counted, so
    every reblock() undoes any number of unblock() calls.
*/

/*!
    \fn void QSignalBlocker::unblock()

    Temporarily restores the QObject::signalsBlocked() state to what
    it was before this QSignalBlocker's constructor ran. To undo, use
    reblock().

    The numbers of reblock() and unblock() calls are not counted, so
    every unblock() undoes any number of reblock() calls.
*/

/*!
    \class QObject
    \inmodule QtCore
    \brief The QObject class is the base class of all Qt objects.

    \ingroup objectmodel

    \reentrant

    QObject is the heart of the Qt \l{Object Model}. The central
    feature in this model is a very powerful mechanism for seamless
    object communication called \l{signals and slots}. You can
    connect a signal to a slot with connect() and destroy the
    connection with disconnect(). To avoid never ending notification
    loops you can temporarily block signals with blockSignals(). The
    protected functions connectNotify() and disconnectNotify() make
    it possible to track connections.

    QObjects organize themselves in \l {Object Trees & Ownership}
    {object trees}. When you create a QObject with another object as
    parent, the object will automatically add itself to the parent's
    children() list. The parent takes ownership of the object; i.e.,
    it will automatically delete its children in its destructor. You
    can look for an object by name and optionally type using
    findChild() or findChildren().

    Every object has an objectName() and its class name can be found
    via the corresponding metaObject() (see QMetaObject::className()).
    You can determine whether the object's class inherits another
    class in the QObject inheritance hierarchy by using the
    inherits() function.

    When an object is deleted, it emits a destroyed() signal. You can
    catch this signal to avoid dangling references to QObjects.

    QObjects can receive events through event() and filter the events
    of other objects. See installEventFilter() and eventFilter() for
    details. A convenience handler, childEvent(), can be reimplemented
    to catch child events.

    Last but not least, QObject provides the basic timer support in
    Qt; see QTimer for high-level support for timers.

    Notice that the Q_OBJECT macro is mandatory for any object that
    implements signals, slots or properties. You also need to run the
    \l{moc}{Meta Object Compiler} on the source file. We strongly
    recommend the use of this macro in all subclasses of QObject
    regardless of whether or not they actually use signals, slots and
    properties, since failure to do so may lead certain functions to
    exhibit strange behavior.

    All Qt widgets inherit QObject. The convenience function
    isWidgetType() returns whether an object is actually a widget. It
    is much faster than
    \l{qobject_cast()}{qobject_cast}<QWidget *>(\e{obj}) or
    \e{obj}->\l{inherits()}{inherits}("QWidget").

    Some QObject functions, e.g. children(), return a QObjectList.
    QObjectList is a typedef for QList<QObject *>.

    \section1 Thread Affinity

    A QObject instance is said to have a \e{thread affinity}, or that
    it \e{lives} in a certain thread. When a QObject receives a
    \l{Qt::QueuedConnection}{queued signal} or a \l{The Event
    System#Sending Events}{posted event}, the slot or event handler
    will run in the thread that the object lives in.

    \note If a QObject has no thread affinity (that is, if thread()
    returns zero), or if it lives in a thread that has no running event
    loop, then it cannot receive queued signals or posted events.

    By default, a QObject lives in the thread in which it is created.
    An object's thread affinity can be queried using thread() and
    changed using moveToThread().

    All QObjects must live in the same thread as their parent. Consequently:

    \list
    \li setParent() will fail if the two QObjects involved live in
        different threads.
    \li When a QObject is moved to another thread, all its children
        will be automatically moved too.
    \li moveToThread() will fail if the QObject has a parent.
    \li If QObjects are created within QThread::run(), they cannot
        become children of the QThread object because the QThread does
        not live in the thread that calls QThread::run().
    \endlist

    \note A QObject's member variables \e{do not} automatically become
    its children. The parent-child relationship must be set by either
    passing a pointer to the child's \l{QObject()}{constructor}, or by
    calling setParent(). Without this step, the object's member variables
    will remain in the old thread when moveToThread() is called.

    \target No copy constructor
    \section1 No Copy Constructor or Assignment Operator

    QObject has neither a copy constructor nor an assignment operator.
    This is by design. Actually, they are declared, but in a
    \c{private} section with the macro Q_DISABLE_COPY(). In fact, all
    Qt classes derived from QObject (direct or indirect) use this
    macro to declare their copy constructor and assignment operator to
    be private. The reasoning is found in the discussion on
    \l{Identity vs Value} {Identity vs Value} on the Qt \l{Object
    Model} page.

    The main consequence is that you should use pointers to QObject
    (or to your QObject subclass) where you might otherwise be tempted
    to use your QObject subclass as a value. For example, without a
    copy constructor, you can't use a subclass of QObject as the value
    to be stored in one of the container classes. You must store
    pointers.

    \section1 Auto-Connection

    Qt's meta-object system provides a mechanism to automatically connect
    signals and slots between QObject subclasses and their children. As long
    as objects are defined with suitable object names, and slots follow a
    simple naming convention, this connection can be performed at run-time
    by the QMetaObject::connectSlotsByName() function.

    \l uic generates code that invokes this function to enable
    auto-connection to be performed between widgets on forms created
    with \e{Qt Designer}. More information about using auto-connection with \e{Qt Designer} is
    given in the \l{Using a Designer UI File in Your C++ Application} section of
    the \e{Qt Designer} manual.

    \section1 Dynamic Properties

    From Qt 4.2, dynamic properties can be added to and removed from QObject
    instances at run-time. Dynamic properties do not need to be declared at
    compile-time, yet they provide the same advantages as static properties
    and are manipulated using the same API - using property() to read them
    and setProperty() to write them.

    From Qt 4.3, dynamic properties are supported by
    \l{Qt Designer's Widget Editing Mode#The Property Editor}{Qt Designer},
    and both standard Qt widgets and user-created forms can be given dynamic
    properties.

    \section1 Internationalization (I18n)

    All QObject subclasses support Qt's translation features, making it possible
    to translate an application's user interface into different languages.

    To make user-visible text translatable, it must be wrapped in calls to
    the tr() function. This is explained in detail in the
    \l{Writing Source Code for Translation} document.

    \sa QMetaObject, QPointer, QObjectCleanupHandler, Q_DISABLE_COPY()
    \sa {Object Trees & Ownership}
*/

/*****************************************************************************
  QObject member functions
 *****************************************************************************/

// check the constructor's parent thread argument
static bool check_parent_thread(QObject *parent,
                                QThreadData *parentThreadData,
                                QThreadData *currentThreadData)
{
    if (parent && parentThreadData != currentThreadData) {
        QThread *parentThread = parentThreadData->thread.loadAcquire();
        QThread *currentThread = currentThreadData->thread.loadAcquire();
        qWarning("QObject: Cannot create children for a parent that is in a different thread.\n"
                 "(Parent is %s(%p), parent's thread is %s(%p), current thread is %s(%p)",
                 parent->metaObject()->className(),
                 parent,
                 parentThread ? parentThread->metaObject()->className() : "QThread",
                 parentThread,
                 currentThread ? currentThread->metaObject()->className() : "QThread",
                 currentThread);
        return false;
    }
    return true;
}

/*!
    Constructs an object with parent object \a parent.

    The parent of an object may be viewed as the object's owner. For
    instance, a \l{QDialog}{dialog box} is the parent of the \uicontrol{OK}
    and \uicontrol{Cancel} buttons it contains.

    The destructor of a parent object destroys all child objects.

    Setting \a parent to \nullptr constructs an object with no parent. If the
    object is a widget, it will become a top-level window.

    \sa parent(), findChild(), findChildren()
*/

QObject::QObject(QObject *parent)
    : QObject(*new QObjectPrivate, parent)
{
}

/*!
    \internal
 */
QObject::QObject(QObjectPrivate &dd, QObject *parent)
    : d_ptr(&dd)
{
    Q_ASSERT_X(this != parent, Q_FUNC_INFO, "Cannot parent a QObject to itself");

    Q_D(QObject);
    d_ptr->q_ptr = this;
    auto threadData = (parent && !parent->thread()) ? parent->d_func()->threadData.loadRelaxed() : QThreadData::current();
    threadData->ref();
    d->threadData.storeRelaxed(threadData);
    if (parent) {
        QT_TRY {
            if (!check_parent_thread(parent, parent ? parent->d_func()->threadData.loadRelaxed() : nullptr, threadData))
                parent = nullptr;
            if (d->isWidget) {
                if (parent) {
                    d->parent = parent;
                    d->parent->d_func()->children.append(this);
                }
                // no events sent here, this is done at the end of the QWidget constructor
            } else {
                setParent(parent);
            }
        } QT_CATCH(...) {
            threadData->deref();
            QT_RETHROW;
        }
    }
#if QT_VERSION < 0x60000
    qt_addObject(this);
#endif
    if (Q_UNLIKELY(qtHookData[QHooks::AddQObject]))
        reinterpret_cast<QHooks::AddQObjectCallback>(qtHookData[QHooks::AddQObject])(this);
    Q_TRACE(QObject_ctor, this);
}

/*!
    Destroys the object, deleting all its child objects.

    All signals to and from the object are automatically disconnected, and
    any pending posted events for the object are removed from the event
    queue. However, it is often safer to use deleteLater() rather than
    deleting a QObject subclass directly.

    \warning All child objects are deleted. If any of these objects
    are on the stack or global, sooner or later your program will
    crash. We do not recommend holding pointers to child objects from
    outside the parent. If you still do, the destroyed() signal gives
    you an opportunity to detect when an object is destroyed.

    \warning Deleting a QObject while pending events are waiting to
    be delivered can cause a crash. You must not delete the QObject
    directly if it exists in a different thread than the one currently
    executing. Use deleteLater() instead, which will cause the event
    loop to delete the object after all pending events have been
    delivered to it.

    \sa deleteLater()
*/

QObject::~QObject()
{
    Q_D(QObject);
    d->wasDeleted = true;
    d->blockSig = 0; // unblock signals so we always emit destroyed()

    QtSharedPointer::ExternalRefCountData *sharedRefcount = d->sharedRefcount.loadRelaxed();
    if (sharedRefcount) {
        if (sharedRefcount->strongref.loadRelaxed() > 0) {
            qWarning("QObject: shared QObject was deleted directly. The program is malformed and may crash.");
            // but continue deleting, it's too late to stop anyway
        }

        // indicate to all QWeakPointers that this QObject has now been deleted
        sharedRefcount->strongref.storeRelaxed(0);
        if (!sharedRefcount->weakref.deref())
            delete sharedRefcount;
    }

    if (!d->isWidget && d->isSignalConnected(0)) {
        emit destroyed(this);
    }

    if (!d->isDeletingChildren && d->declarativeData) {
        if (static_cast<QAbstractDeclarativeDataImpl*>(d->declarativeData)->ownedByQml1) {
            if (QAbstractDeclarativeData::destroyed_qml1)
                QAbstractDeclarativeData::destroyed_qml1(d->declarativeData, this);
        } else {
            if (QAbstractDeclarativeData::destroyed)
                QAbstractDeclarativeData::destroyed(d->declarativeData, this);
        }
    }

    QObjectPrivate::ConnectionData *cd = d->connections.loadRelaxed();
    if (cd) {
        if (cd->currentSender) {
            cd->currentSender->receiverDeleted();
            cd->currentSender = nullptr;
        }

        QBasicMutex *signalSlotMutex = signalSlotLock(this);
        QBasicMutexLocker locker(signalSlotMutex);

        // disconnect all receivers
        int receiverCount = cd->signalVectorCount();
        for (int signal = -1; signal < receiverCount; ++signal) {
            QObjectPrivate::ConnectionList &connectionList = cd->connectionsForSignal(signal);

            while (QObjectPrivate::Connection *c = connectionList.first.loadRelaxed()) {
                Q_ASSERT(c->receiver.loadAcquire());

                QBasicMutex *m = signalSlotLock(c->receiver.loadRelaxed());
                bool needToUnlock = QOrderedMutexLocker::relock(signalSlotMutex, m);
                if (c == connectionList.first.loadAcquire() && c->receiver.loadAcquire()) {
                    cd->removeConnection(c);
                    Q_ASSERT(connectionList.first.loadRelaxed() != c);
                }
                if (needToUnlock)
                    m->unlock();
            }
        }

        /* Disconnect all senders:
         */
        while (QObjectPrivate::Connection *node = cd->senders) {
            Q_ASSERT(node->receiver.loadAcquire());
            QObject *sender = node->sender;
            // Send disconnectNotify before removing the connection from sender's connection list.
            // This ensures any eventual destructor of sender will block on getting receiver's lock
            // and not finish until we release it.
            sender->disconnectNotify(QMetaObjectPrivate::signal(sender->metaObject(), node->signal_index));
            QBasicMutex *m = signalSlotLock(sender);
            bool needToUnlock = QOrderedMutexLocker::relock(signalSlotMutex, m);
            //the node has maybe been removed while the mutex was unlocked in relock?
            if (node != cd->senders) {
                // We hold the wrong mutex
                Q_ASSERT(needToUnlock);
                m->unlock();
                continue;
            }

            QObjectPrivate::ConnectionData *senderData = sender->d_func()->connections.loadRelaxed();
            Q_ASSERT(senderData);

            QtPrivate::QSlotObjectBase *slotObj = nullptr;
            if (node->isSlotObject) {
                slotObj = node->slotObj;
                node->isSlotObject = false;
            }

            senderData->removeConnection(node);
            /*
              When we unlock, another thread has the chance to delete/modify sender data.
              Thus we need to call cleanOrphanedConnections before unlocking. We use the
              variant of the function which assumes that the lock is already held to avoid
              a deadlock.
              We need to hold m, the sender lock. Considering that we might execute arbitrary user
              code, we should already release the signalSlotMutex here – unless they are the same.
            */
            const bool locksAreTheSame = signalSlotMutex == m;
            if (!locksAreTheSame)
                locker.unlock();
            senderData->cleanOrphanedConnections(
                        sender,
                        QObjectPrivate::ConnectionData::AlreadyLockedAndTemporarilyReleasingLock
                        );
            if (needToUnlock)
                m->unlock();

            if (locksAreTheSame) // otherwise already unlocked
                locker.unlock();
            if (slotObj)
                slotObj->destroyIfLastRef();
            locker.relock();
        }

        // invalidate all connections on the object and make sure
        // activate() will skip them
        cd->currentConnectionId.storeRelaxed(0);
    }
    if (cd && !cd->ref.deref())
        delete cd;
    d->connections.storeRelaxed(nullptr);

    if (!d->children.isEmpty())
        d->deleteChildren();

#if QT_VERSION < 0x60000
    qt_removeObject(this);
#endif
    if (Q_UNLIKELY(qtHookData[QHooks::RemoveQObject]))
        reinterpret_cast<QHooks::RemoveQObjectCallback>(qtHookData[QHooks::RemoveQObject])(this);

    Q_TRACE(QObject_dtor, this);

    if (d->parent)        // remove it from parent object
        d->setParent_helper(nullptr);
}

QObjectPrivate::Connection::~Connection()
{
    if (ownArgumentTypes) {
        const int *v = argumentTypes.loadRelaxed();
        if (v != &DIRECT_CONNECTION_ONLY)
            delete [] v;
    }
    if (isSlotObject)
        slotObj->destroyIfLastRef();
}


/*!
    \fn const QMetaObject *QObject::metaObject() const

    Returns a pointer to the meta-object of this object.

    A meta-object contains information about a class that inherits
    QObject, e.g. class name, superclass name, properties, signals and
    slots. Every QObject subclass that contains the Q_OBJECT macro will have a
    meta-object.

    The meta-object information is required by the signal/slot
    connection mechanism and the property system. The inherits()
    function also makes use of the meta-object.

    If you have no pointer to an actual object instance but still
    want to access the meta-object of a class, you can use \l
    staticMetaObject.

    Example:

    \snippet code/src_corelib_kernel_qobject.cpp 1

    \sa staticMetaObject
*/

/*!
    \variable QObject::staticMetaObject

    This variable stores the meta-object for the class.

    A meta-object contains information about a class that inherits
    QObject, e.g. class name, superclass name, properties, signals and
    slots. Every class that contains the Q_OBJECT macro will also have
    a meta-object.

    The meta-object information is required by the signal/slot
    connection mechanism and the property system. The inherits()
    function also makes use of the meta-object.

    If you have a pointer to an object, you can use metaObject() to
    retrieve the meta-object associated with that object.

    Example:

    \snippet code/src_corelib_kernel_qobject.cpp 2

    \sa metaObject()
*/

/*!
    \fn template <class T> T qobject_cast(QObject *object)
    \fn template <class T> T qobject_cast(const QObject *object)
    \relates QObject

    Returns the given \a object cast to type T if the object is of type
    T (or of a subclass); otherwise returns \nullptr. If \a object is
    \nullptr then it will also return \nullptr.

    The class T must inherit (directly or indirectly) QObject and be
    declared with the \l Q_OBJECT macro.

    A class is considered to inherit itself.

    Example:

    \snippet code/src_corelib_kernel_qobject.cpp 3

    The qobject_cast() function behaves similarly to the standard C++
    \c dynamic_cast(), with the advantages that it doesn't require
    RTTI support and it works across dynamic library boundaries.

    qobject_cast() can also be used in conjunction with interfaces;
    see the \l{tools/plugandpaint/app}{Plug & Paint} example for details.

    \warning If T isn't declared with the Q_OBJECT macro, this
    function's return value is undefined.

    \sa QObject::inherits()
*/

/*!
    \fn bool QObject::inherits(const char *className) const

    Returns \c true if this object is an instance of a class that
    inherits \a className or a QObject subclass that inherits \a
    className; otherwise returns \c false.

    A class is considered to inherit itself.

    Example:

    \snippet code/src_corelib_kernel_qobject.cpp 4

    If you need to determine whether an object is an instance of a particular
    class for the purpose of casting it, consider using qobject_cast<Type *>(object)
    instead.

    \sa metaObject(), qobject_cast()
*/

/*!
    \property QObject::objectName

    \brief the name of this object

    You can find an object by name (and type) using findChild().
    You can find a set of objects with findChildren().

    \snippet code/src_corelib_kernel_qobject.cpp 5

    By default, this property contains an empty string.

    \sa metaObject(), QMetaObject::className()
*/

QString QObject::objectName() const
{
    Q_D(const QObject);
    return d->extraData ? d->extraData->objectName : QString();
}

/*
    Sets the object's name to \a name.
*/
void QObject::setObjectName(const QString &name)
{
    Q_D(QObject);
    if (!d->extraData)
        d->extraData = new QObjectPrivate::ExtraData;

    if (d->extraData->objectName != name) {
        d->extraData->objectName = name;
        emit objectNameChanged(d->extraData->objectName, QPrivateSignal());
    }
}

/*! \fn void QObject::objectNameChanged(const QString &objectName)

    This signal is emitted after the object's name has been changed. The new object name is passed as \a objectName.

    \sa QObject::objectName
*/

/*!
    \fn bool QObject::isWidgetType() const

    Returns \c true if the object is a widget; otherwise returns \c false.

    Calling this function is equivalent to calling
    \c{inherits("QWidget")}, except that it is much faster.
*/

/*!
    \fn bool QObject::isWindowType() const

    Returns \c true if the object is a window; otherwise returns \c false.

    Calling this function is equivalent to calling
    \c{inherits("QWindow")}, except that it is much faster.
*/

/*!
    This virtual function receives events to an object and should
    return true if the event \a e was recognized and processed.

    The event() function can be reimplemented to customize the
    behavior of an object.

    Make sure you call the parent event class implementation
    for all the events you did not handle.

    Example:

    \snippet code/src_corelib_kernel_qobject.cpp 52

    \sa installEventFilter(), timerEvent(), QCoreApplication::sendEvent(),
    QCoreApplication::postEvent()
*/

bool QObject::event(QEvent *e)
{
    switch (e->type()) {
    case QEvent::Timer:
        timerEvent((QTimerEvent*)e);
        break;

    case QEvent::ChildAdded:
    case QEvent::ChildPolished:
    case QEvent::ChildRemoved:
        childEvent((QChildEvent*)e);
        break;

    case QEvent::DeferredDelete:
        qDeleteInEventHandler(this);
        break;

    case QEvent::MetaCall:
        {
            QAbstractMetaCallEvent *mce = static_cast<QAbstractMetaCallEvent*>(e);

            if (!d_func()->connections.loadRelaxed()) {
                QBasicMutexLocker locker(signalSlotLock(this));
                d_func()->ensureConnectionData();
            }
            QObjectPrivate::Sender sender(this, const_cast<QObject*>(mce->sender()), mce->signalId());

            mce->placeMetaCall(this);
            break;
        }

    case QEvent::ThreadChange: {
        Q_D(QObject);
        QThreadData *threadData = d->threadData.loadRelaxed();
        QAbstractEventDispatcher *eventDispatcher = threadData->eventDispatcher.loadRelaxed();
        if (eventDispatcher) {
            QList<QAbstractEventDispatcher::TimerInfo> timers = eventDispatcher->registeredTimers(this);
            if (!timers.isEmpty()) {
                // do not to release our timer ids back to the pool (since the timer ids are moving to a new thread).
                eventDispatcher->unregisterTimers(this);
                QMetaObject::invokeMethod(this, "_q_reregisterTimers", Qt::QueuedConnection,
                                          Q_ARG(void*, (new QList<QAbstractEventDispatcher::TimerInfo>(timers))));
            }
        }
        break;
    }

    default:
        if (e->type() >= QEvent::User) {
            customEvent(e);
            break;
        }
        return false;
    }
    return true;
}

/*!
    \fn void QObject::timerEvent(QTimerEvent *event)

    This event handler can be reimplemented in a subclass to receive
    timer events for the object.

    QTimer provides a higher-level interface to the timer
    functionality, and also more general information about timers. The
    timer event is passed in the \a event parameter.

    \sa startTimer(), killTimer(), event()
*/

void QObject::timerEvent(QTimerEvent *)
{
}


/*!
    This event handler can be reimplemented in a subclass to receive
    child events. The event is passed in the \a event parameter.

    QEvent::ChildAdded and QEvent::ChildRemoved events are sent to
    objects when children are added or removed. In both cases you can
    only rely on the child being a QObject, or if isWidgetType()
    returns \c true, a QWidget. (This is because, in the
    \l{QEvent::ChildAdded}{ChildAdded} case, the child is not yet
    fully constructed, and in the \l{QEvent::ChildRemoved}{ChildRemoved}
    case it might have been destructed already).

    QEvent::ChildPolished events are sent to widgets when children
    are polished, or when polished children are added. If you receive
    a child polished event, the child's construction is usually
    completed. However, this is not guaranteed, and multiple polish
    events may be delivered during the execution of a widget's
    constructor.

    For every child widget, you receive one
    \l{QEvent::ChildAdded}{ChildAdded} event, zero or more
    \l{QEvent::ChildPolished}{ChildPolished} events, and one
    \l{QEvent::ChildRemoved}{ChildRemoved} event.

    The \l{QEvent::ChildPolished}{ChildPolished} event is omitted if
    a child is removed immediately after it is added. If a child is
    polished several times during construction and destruction, you
    may receive several child polished events for the same child,
    each time with a different virtual table.

    \sa event()
*/

void QObject::childEvent(QChildEvent * /* event */)
{
}


/*!
    This event handler can be reimplemented in a subclass to receive
    custom events. Custom events are user-defined events with a type
    value at least as large as the QEvent::User item of the
    QEvent::Type enum, and is typically a QEvent subclass. The event
    is passed in the \a event parameter.

    \sa event(), QEvent
*/
void QObject::customEvent(QEvent * /* event */)
{
}



/*!
    Filters events if this object has been installed as an event
    filter for the \a watched object.

    In your reimplementation of this function, if you want to filter
    the \a event out, i.e. stop it being handled further, return
    true; otherwise return false.

    Example:
    \snippet code/src_corelib_kernel_qobject.cpp 6

    Notice in the example above that unhandled events are passed to
    the base class's eventFilter() function, since the base class
    might have reimplemented eventFilter() for its own internal
    purposes.

    Some events, such as \l QEvent::ShortcutOverride must be explicitly
    accepted (by calling \l {QEvent::}{accept()} on them) in order to prevent
    propagation.

    \warning If you delete the receiver object in this function, be
    sure to return true. Otherwise, Qt will forward the event to the
    deleted object and the program might crash.

    \sa installEventFilter()
*/

bool QObject::eventFilter(QObject * /* watched */, QEvent * /* event */)
{
    return false;
}

/*!
    \fn bool QObject::signalsBlocked() const

    Returns \c true if signals are blocked; otherwise returns \c false.

    Signals are not blocked by default.

    \sa blockSignals(), QSignalBlocker
*/

/*!
    If \a block is true, signals emitted by this object are blocked
    (i.e., emitting a signal will not invoke anything connected to it).
    If \a block is false, no such blocking will occur.

    The return value is the previous value of signalsBlocked().

    Note that the destroyed() signal will be emitted even if the signals
    for this object have been blocked.

    Signals emitted while being blocked are not buffered.

    \sa signalsBlocked(), QSignalBlocker
*/

bool QObject::blockSignals(bool block) noexcept
{
    Q_D(QObject);
    bool previous = d->blockSig;
    d->blockSig = block;
    return previous;
}

/*!
    Returns the thread in which the object lives.

    \sa moveToThread()
*/
QThread *QObject::thread() const
{
    return d_func()->threadData.loadRelaxed()->thread.loadAcquire();
}

/*!
    Changes the thread affinity for this object and its children. The
    object cannot be moved if it has a parent. Event processing will
    continue in the \a targetThread.

    To move an object to the main thread, use QApplication::instance()
    to retrieve a pointer to the current application, and then use
    QApplication::thread() to retrieve the thread in which the
    application lives. For example:

    \snippet code/src_corelib_kernel_qobject.cpp 7

    If \a targetThread is \nullptr, all event processing for this object
    and its children stops, as they are no longer associated with any
    thread.

    Note that all active timers for the object will be reset. The
    timers are first stopped in the current thread and restarted (with
    the same interval) in the \a targetThread. As a result, constantly
    moving an object between threads can postpone timer events
    indefinitely.

    A QEvent::ThreadChange event is sent to this object just before
    the thread affinity is changed. You can handle this event to
    perform any special processing. Note that any new events that are
    posted to this object will be handled in the \a targetThread,
    provided it is not \nullptr: when it is \nullptr, no event processing
    for this object or its children can happen, as they are no longer
    associated with any thread.

    \warning This function is \e not thread-safe; the current thread
    must be same as the current thread affinity. In other words, this
    function can only "push" an object from the current thread to
    another thread, it cannot "pull" an object from any arbitrary
    thread to the current thread. There is one exception to this rule
    however: objects with no thread affinity can be "pulled" to the
    current thread.

    \sa thread()
 */
void QObject::moveToThread(QThread *targetThread)
{
    Q_D(QObject);

    if (d->threadData.loadRelaxed()->thread.loadAcquire() == targetThread) {
        // object is already in this thread
        return;
    }

    if (d->parent != nullptr) {
        qWarning("QObject::moveToThread: Cannot move objects with a parent");
        return;
    }
    if (d->isWidget) {
        qWarning("QObject::moveToThread: Widgets cannot be moved to a new thread");
        return;
    }

    QThreadData *currentData = QThreadData::current();
    QThreadData *targetData = targetThread ? QThreadData::get2(targetThread) : nullptr;
    QThreadData *thisThreadData = d->threadData.loadAcquire();
    if (!thisThreadData->thread.loadAcquire() && currentData == targetData) {
        // one exception to the rule: we allow moving objects with no thread affinity to the current thread
        currentData = d->threadData;
    } else if (thisThreadData != currentData) {
        qWarning("QObject::moveToThread: Current thread (%p) is not the object's thread (%p).\n"
                 "Cannot move to target thread (%p)\n",
                 currentData->thread.loadRelaxed(), thisThreadData->thread.loadRelaxed(), targetData ? targetData->thread.loadRelaxed() : nullptr);

#ifdef Q_OS_MAC
        qWarning("You might be loading two sets of Qt binaries into the same process. "
                 "Check that all plugins are compiled against the right Qt binaries. Export "
                 "DYLD_PRINT_LIBRARIES=1 and check that only one set of binaries are being loaded.");
#endif

        return;
    }

    // prepare to move
    d->moveToThread_helper();

    if (!targetData)
        targetData = new QThreadData(0);

    // make sure nobody adds/removes connections to this object while we're moving it
    QMutexLocker l(signalSlotLock(this));

    QOrderedMutexLocker locker(&currentData->postEventList.mutex,
                               &targetData->postEventList.mutex);

    // keep currentData alive (since we've got it locked)
    currentData->ref();

    // move the object
    d_func()->setThreadData_helper(currentData, targetData);

    locker.unlock();

    // now currentData can commit suicide if it wants to
    currentData->deref();
}

void QObjectPrivate::moveToThread_helper()
{
    Q_Q(QObject);
    QEvent e(QEvent::ThreadChange);
    QCoreApplication::sendEvent(q, &e);
    for (int i = 0; i < children.size(); ++i) {
        QObject *child = children.at(i);
        child->d_func()->moveToThread_helper();
    }
}

void QObjectPrivate::setThreadData_helper(QThreadData *currentData, QThreadData *targetData)
{
    Q_Q(QObject);

    // move posted events
    int eventsMoved = 0;
    for (int i = 0; i < currentData->postEventList.size(); ++i) {
        const QPostEvent &pe = currentData->postEventList.at(i);
        if (!pe.event)
            continue;
        if (pe.receiver == q) {
            // move this post event to the targetList
            targetData->postEventList.addEvent(pe);
            const_cast<QPostEvent &>(pe).event = nullptr;
            ++eventsMoved;
        }
    }
    if (eventsMoved > 0 && targetData->hasEventDispatcher()) {
        targetData->canWait = false;
        targetData->eventDispatcher.loadRelaxed()->wakeUp();
    }

    // the current emitting thread shouldn't restore currentSender after calling moveToThread()
    ConnectionData *cd = connections.loadRelaxed();
    if (cd) {
        if (cd->currentSender) {
            cd->currentSender->receiverDeleted();
            cd->currentSender = nullptr;
        }

        // adjust the receiverThreadId values in the Connections
        if (cd) {
            auto *c = cd->senders;
            while (c) {
                QObject *r = c->receiver.loadRelaxed();
                if (r) {
                    Q_ASSERT(r == q);
                    targetData->ref();
                    QThreadData *old = c->receiverThreadData.loadRelaxed();
                    if (old)
                        old->deref();
                    c->receiverThreadData.storeRelaxed(targetData);
                }
                c = c->next;
            }
        }

    }

    // set new thread data
    targetData->ref();
    threadData.loadRelaxed()->deref();

    // synchronizes with loadAcquire e.g. in QCoreApplication::postEvent
    threadData.storeRelease(targetData);

    for (int i = 0; i < children.size(); ++i) {
        QObject *child = children.at(i);
        child->d_func()->setThreadData_helper(currentData, targetData);
    }
}

void QObjectPrivate::_q_reregisterTimers(void *pointer)
{
    Q_Q(QObject);
    QList<QAbstractEventDispatcher::TimerInfo> *timerList = reinterpret_cast<QList<QAbstractEventDispatcher::TimerInfo> *>(pointer);
    QAbstractEventDispatcher *eventDispatcher = threadData.loadRelaxed()->eventDispatcher.loadRelaxed();
    for (int i = 0; i < timerList->size(); ++i) {
        const QAbstractEventDispatcher::TimerInfo &ti = timerList->at(i);
        eventDispatcher->registerTimer(ti.timerId, ti.interval, ti.timerType, q);
    }
    delete timerList;
}


//
// The timer flag hasTimer is set when startTimer is called.
// It is not reset when killing the timer because more than
// one timer might be active.
//

/*!
    Starts a timer and returns a timer identifier, or returns zero if
    it could not start a timer.

    A timer event will occur every \a interval milliseconds until
    killTimer() is called. If \a interval is 0, then the timer event
    occurs once every time there are no more window system events to
    process.

    The virtual timerEvent() function is called with the QTimerEvent
    event parameter class when a timer event occurs. Reimplement this
    function to get timer events.

    If multiple timers are running, the QTimerEvent::timerId() can be
    used to find out which timer was activated.

    Example:

    \snippet code/src_corelib_kernel_qobject.cpp 8

    Note that QTimer's accuracy depends on the underlying operating system and
    hardware. The \a timerType argument allows you to customize the accuracy of
    the timer. See Qt::TimerType for information on the different timer types.
    Most platforms support an accuracy of 20 milliseconds; some provide more.
    If Qt is unable to deliver the requested number of timer events, it will
    silently discard some.

    The QTimer class provides a high-level programming interface with
    single-shot timers and timer signals instead of events. There is
    also a QBasicTimer class that is more lightweight than QTimer and
    less clumsy than using timer IDs directly.

    \sa timerEvent(), killTimer(), QTimer::singleShot()
*/

int QObject::startTimer(int interval, Qt::TimerType timerType)
{
    Q_D(QObject);

    if (Q_UNLIKELY(interval < 0)) {
        qWarning("QObject::startTimer: Timers cannot have negative intervals");
        return 0;
    }

    auto thisThreadData = d->threadData.loadRelaxed();
    if (Q_UNLIKELY(!thisThreadData->hasEventDispatcher())) {
        qWarning("QObject::startTimer: Timers can only be used with threads started with QThread");
        return 0;
    }
    if (Q_UNLIKELY(thread() != QThread::currentThread())) {
        qWarning("QObject::startTimer: Timers cannot be started from another thread");
        return 0;
    }
    int timerId = thisThreadData->eventDispatcher.loadRelaxed()->registerTimer(interval, timerType, this);
    if (!d->extraData)
        d->extraData = new QObjectPrivate::ExtraData;
    d->extraData->runningTimers.append(timerId);
    return timerId;
}

/*!
    \since 5.9
    \overload
    \fn int QObject::startTimer(std::chrono::milliseconds time, Qt::TimerType timerType)

    Starts a timer and returns a timer identifier, or returns zero if
    it could not start a timer.

    A timer event will occur every \a time interval until killTimer()
    is called. If \a time is equal to \c{std::chrono::duration::zero()},
    then the timer event occurs once every time there are no more window
    system events to process.

    The virtual timerEvent() function is called with the QTimerEvent
    event parameter class when a timer event occurs. Reimplement this
    function to get timer events.

    If multiple timers are running, the QTimerEvent::timerId() can be
    used to find out which timer was activated.

    Example:

    \snippet code/src_corelib_kernel_qobject.cpp 8

    Note that QTimer's accuracy depends on the underlying operating system and
    hardware. The \a timerType argument allows you to customize the accuracy of
    the timer. See Qt::TimerType for information on the different timer types.
    Most platforms support an accuracy of 20 milliseconds; some provide more.
    If Qt is unable to deliver the requested number of timer events, it will
    silently discard some.

    The QTimer class provides a high-level programming interface with
    single-shot timers and timer signals instead of events. There is
    also a QBasicTimer class that is more lightweight than QTimer and
    less clumsy than using timer IDs directly.

    \sa timerEvent(), killTimer(), QTimer::singleShot()
*/

/*!
    Kills the timer with timer identifier, \a id.

    The timer identifier is returned by startTimer() when a timer
    event is started.

    \sa timerEvent(), startTimer()
*/

void QObject::killTimer(int id)
{
    Q_D(QObject);
    if (Q_UNLIKELY(thread() != QThread::currentThread())) {
        qWarning("QObject::killTimer: Timers cannot be stopped from another thread");
        return;
    }
    if (id) {
        int at = d->extraData ? d->extraData->runningTimers.indexOf(id) : -1;
        if (at == -1) {
            // timer isn't owned by this object
            qWarning("QObject::killTimer(): Error: timer id %d is not valid for object %p (%s, %ls), timer has not been killed",
                     id,
                     this,
                     metaObject()->className(),
                     qUtf16Printable(objectName()));
            return;
        }

        auto thisThreadData = d->threadData.loadRelaxed();
        if (thisThreadData->hasEventDispatcher())
            thisThreadData->eventDispatcher.loadRelaxed()->unregisterTimer(id);

        d->extraData->runningTimers.remove(at);
        QAbstractEventDispatcherPrivate::releaseTimerId(id);
    }
}


/*!
    \fn QObject *QObject::parent() const

    Returns a pointer to the parent object.

    \sa children()
*/

/*!
    \fn const QObjectList &QObject::children() const

    Returns a list of child objects.
    The QObjectList class is defined in the \c{<QObject>} header
    file as the following:

    \quotefromfile kernel/qobject.h
    \skipto /typedef .*QObjectList/
    \printuntil QObjectList

    The first child added is the \l{QList::first()}{first} object in
    the list and the last child added is the \l{QList::last()}{last}
    object in the list, i.e. new children are appended at the end.

    Note that the list order changes when QWidget children are
    \l{QWidget::raise()}{raised} or \l{QWidget::lower()}{lowered}. A
    widget that is raised becomes the last object in the list, and a
    widget that is lowered becomes the first object in the list.

    \sa findChild(), findChildren(), parent(), setParent()
*/


/*!
    \fn template<typename T> T *QObject::findChild(const QString &name, Qt::FindChildOptions options) const

    Returns the child of this object that can be cast into type T and
    that is called \a name, or \nullptr if there is no such object.
    Omitting the \a name argument causes all object names to be matched.
    The search is performed recursively, unless \a options specifies the
    option FindDirectChildrenOnly.

    If there is more than one child matching the search, the most
    direct ancestor is returned. If there are several direct
    ancestors, it is undefined which one will be returned. In that
    case, findChildren() should be used.

    This example returns a child \c{QPushButton} of \c{parentWidget}
    named \c{"button1"}, even if the button isn't a direct child of
    the parent:

    \snippet code/src_corelib_kernel_qobject.cpp 10

    This example returns a \c{QListWidget} child of \c{parentWidget}:

    \snippet code/src_corelib_kernel_qobject.cpp 11

    This example returns a child \c{QPushButton} of \c{parentWidget}
    (its direct parent) named \c{"button1"}:

    \snippet code/src_corelib_kernel_qobject.cpp 41

    This example returns a \c{QListWidget} child of \c{parentWidget},
    its direct parent:

    \snippet code/src_corelib_kernel_qobject.cpp 42

    \sa findChildren()
*/

/*!
    \fn template<typename T> QList<T> QObject::findChildren(const QString &name, Qt::FindChildOptions options) const

    Returns all children of this object with the given \a name that can be
    cast to type T, or an empty list if there are no such objects.
    Omitting the \a name argument causes all object names to be matched.
    The search is performed recursively, unless \a options specifies the
    option FindDirectChildrenOnly.

    The following example shows how to find a list of child \c{QWidget}s of
    the specified \c{parentWidget} named \c{widgetname}:

    \snippet code/src_corelib_kernel_qobject.cpp 12

    This example returns all \c{QPushButton}s that are children of \c{parentWidget}:

    \snippet code/src_corelib_kernel_qobject.cpp 13

    This example returns all \c{QPushButton}s that are immediate children of \c{parentWidget}:

    \snippet code/src_corelib_kernel_qobject.cpp 43

    \sa findChild()
*/

/*!
    \fn template<typename T> QList<T> QObject::findChildren(const QRegExp &regExp, Qt::FindChildOptions options) const
    \overload findChildren()
    \obsolete

    Returns the children of this object that can be cast to type T
    and that have names matching the regular expression \a regExp,
    or an empty list if there are no such objects.
    The search is performed recursively, unless \a options specifies the
    option FindDirectChildrenOnly.

    Use the findChildren overload taking a QRegularExpression instead.
*/

/*!
    \fn QList<T> QObject::findChildren(const QRegularExpression &re, Qt::FindChildOptions options) const
    \overload findChildren()

    \since 5.0

    Returns the children of this object that can be cast to type T
    and that have names matching the regular expression \a re,
    or an empty list if there are no such objects.
    The search is performed recursively, unless \a options specifies the
    option FindDirectChildrenOnly.
*/

/*!
    \fn template<typename T> T qFindChild(const QObject *obj, const QString &name)
    \relates QObject
    \overload qFindChildren()
    \obsolete

    This function is equivalent to
    \a{obj}->\l{QObject::findChild()}{findChild}<T>(\a name).

    \note This function was provided as a workaround for MSVC 6
    which did not support member template functions. It is advised
    to use the other form in new code.

    \sa QObject::findChild()
*/

/*!
    \fn template<typename T> QList<T> qFindChildren(const QObject *obj, const QString &name)
    \relates QObject
    \overload qFindChildren()
    \obsolete

    This function is equivalent to
    \a{obj}->\l{QObject::findChildren()}{findChildren}<T>(\a name).

    \note This function was provided as a workaround for MSVC 6
    which did not support member template functions. It is advised
    to use the other form in new code.

    \sa QObject::findChildren()
*/

/*!
    \fn template<typename T> QList<T> qFindChildren(const QObject *obj, const QRegExp &regExp)
    \relates QObject
    \overload qFindChildren()

    This function is equivalent to
    \a{obj}->\l{QObject::findChildren()}{findChildren}<T>(\a regExp).

    \note This function was provided as a workaround for MSVC 6
    which did not support member template functions. It is advised
    to use the other form in new code.

    \sa QObject::findChildren()
*/

/*!
    \internal
*/
void qt_qFindChildren_helper(const QObject *parent, const QString &name,
                             const QMetaObject &mo, QList<void*> *list, Qt::FindChildOptions options)
{
    if (!parent || !list)
        return;
    const QObjectList &children = parent->children();
    QObject *obj;
    for (int i = 0; i < children.size(); ++i) {
        obj = children.at(i);
        if (mo.cast(obj)) {
            if (name.isNull() || obj->objectName() == name)
                list->append(obj);
        }
        if (options & Qt::FindChildrenRecursively)
            qt_qFindChildren_helper(obj, name, mo, list, options);
    }
}

#ifndef QT_NO_REGEXP
/*!
    \internal
*/
void qt_qFindChildren_helper(const QObject *parent, const QRegExp &re,
                             const QMetaObject &mo, QList<void*> *list, Qt::FindChildOptions options)
{
    if (!parent || !list)
        return;
    const QObjectList &children = parent->children();
    QRegExp reCopy = re;
    QObject *obj;
    for (int i = 0; i < children.size(); ++i) {
        obj = children.at(i);
        if (mo.cast(obj) && reCopy.indexIn(obj->objectName()) != -1)
            list->append(obj);

        if (options & Qt::FindChildrenRecursively)
            qt_qFindChildren_helper(obj, re, mo, list, options);
    }
}
#endif // QT_NO_REGEXP

#if QT_CONFIG(regularexpression)
/*!
    \internal
*/
void qt_qFindChildren_helper(const QObject *parent, const QRegularExpression &re,
                             const QMetaObject &mo, QList<void*> *list, Qt::FindChildOptions options)
{
    if (!parent || !list)
        return;
    const QObjectList &children = parent->children();
    QObject *obj;
    for (int i = 0; i < children.size(); ++i) {
        obj = children.at(i);
        if (mo.cast(obj)) {
            QRegularExpressionMatch m = re.match(obj->objectName());
            if (m.hasMatch())
                list->append(obj);
        }
        if (options & Qt::FindChildrenRecursively)
            qt_qFindChildren_helper(obj, re, mo, list, options);
    }
}
#endif // QT_CONFIG(regularexpression)

/*!
    \internal
 */
QObject *qt_qFindChild_helper(const QObject *parent, const QString &name, const QMetaObject &mo, Qt::FindChildOptions options)
{
    if (!parent)
        return nullptr;
    const QObjectList &children = parent->children();
    QObject *obj;
    int i;
    for (i = 0; i < children.size(); ++i) {
        obj = children.at(i);
        if (mo.cast(obj) && (name.isNull() || obj->objectName() == name))
            return obj;
    }
    if (options & Qt::FindChildrenRecursively) {
        for (i = 0; i < children.size(); ++i) {
            obj = qt_qFindChild_helper(children.at(i), name, mo, options);
            if (obj)
                return obj;
        }
    }
    return nullptr;
}

/*!
    Makes the object a child of \a parent.

    \sa parent(), children()
*/
void QObject::setParent(QObject *parent)
{
    Q_D(QObject);
    Q_ASSERT(!d->isWidget);
    d->setParent_helper(parent);
}

void QObjectPrivate::deleteChildren()
{
    Q_ASSERT_X(!isDeletingChildren, "QObjectPrivate::deleteChildren()", "isDeletingChildren already set, did this function recurse?");
    isDeletingChildren = true;
    // delete children objects
    // don't use qDeleteAll as the destructor of the child might
    // delete siblings
    for (int i = 0; i < children.count(); ++i) {
        currentChildBeingDeleted = children.at(i);
        children[i] = 0;
        delete currentChildBeingDeleted;
    }
    children.clear();
    currentChildBeingDeleted = nullptr;
    isDeletingChildren = false;
}

void QObjectPrivate::setParent_helper(QObject *o)
{
    Q_Q(QObject);
    Q_ASSERT_X(q != o, Q_FUNC_INFO, "Cannot parent a QObject to itself");
#ifdef QT_DEBUG
    const auto checkForParentChildLoops = qScopeGuard([&](){
        int depth = 0;
        auto p = parent;
        while (p) {
            if (++depth == CheckForParentChildLoopsWarnDepth) {
                qWarning("QObject %p (class: '%s', object name: '%s') may have a loop in its parent-child chain; "
                         "this is undefined behavior",
                         q, q->metaObject()->className(), qPrintable(q->objectName()));
            }
            p = p->parent();
        }
    });
#endif

    if (o == parent)
        return;

    if (parent) {
        QObjectPrivate *parentD = parent->d_func();
        if (parentD->isDeletingChildren && wasDeleted
            && parentD->currentChildBeingDeleted == q) {
            // don't do anything since QObjectPrivate::deleteChildren() already
            // cleared our entry in parentD->children.
        } else {
            const int index = parentD->children.indexOf(q);
            if (index < 0) {
                // we're probably recursing into setParent() from a ChildRemoved event, don't do anything
            } else if (parentD->isDeletingChildren) {
                parentD->children[index] = 0;
            } else {
                parentD->children.removeAt(index);
                if (sendChildEvents && parentD->receiveChildEvents) {
                    QChildEvent e(QEvent::ChildRemoved, q);
                    QCoreApplication::sendEvent(parent, &e);
                }
            }
        }
    }
    parent = o;
    if (parent) {
        // object hierarchies are constrained to a single thread
        if (threadData != parent->d_func()->threadData) {
            qWarning("QObject::setParent: Cannot set parent, new parent is in a different thread");
            parent = nullptr;
            return;
        }
        parent->d_func()->children.append(q);
        if(sendChildEvents && parent->d_func()->receiveChildEvents) {
            if (!isWidget) {
                QChildEvent e(QEvent::ChildAdded, q);
                QCoreApplication::sendEvent(parent, &e);
            }
        }
    }
    if (!wasDeleted && !isDeletingChildren && declarativeData && QAbstractDeclarativeData::parentChanged)
        QAbstractDeclarativeData::parentChanged(declarativeData, q, o);
}

/*!
    \fn void QObject::installEventFilter(QObject *filterObj)

    Installs an event filter \a filterObj on this object. For example:
    \snippet code/src_corelib_kernel_qobject.cpp 14

    An event filter is an object that receives all events that are
    sent to this object. The filter can either stop the event or
    forward it to this object. The event filter \a filterObj receives
    events via its eventFilter() function. The eventFilter() function
    must return true if the event should be filtered, (i.e. stopped);
    otherwise it must return false.

    If multiple event filters are installed on a single object, the
    filter that was installed last is activated first.

    If \a filterObj has already been installed for this object,
    this function moves it so it acts as if it was installed last.

    Here's a \c KeyPressEater class that eats the key presses of its
    monitored objects:

    \snippet code/src_corelib_kernel_qobject.cpp 15

    And here's how to install it on two widgets:

    \snippet code/src_corelib_kernel_qobject.cpp 16

    The QShortcut class, for example, uses this technique to intercept
    shortcut key presses.

    \warning If you delete the receiver object in your eventFilter()
    function, be sure to return true. If you return false, Qt sends
    the event to the deleted object and the program will crash.

    Note that the filtering object must be in the same thread as this
    object. If \a filterObj is in a different thread, this function does
    nothing. If either \a filterObj or this object are moved to a different
    thread after calling this function, the event filter will not be
    called until both objects have the same thread affinity again (it
    is \e not removed).

    \sa removeEventFilter(), eventFilter(), event()
*/

void QObject::installEventFilter(QObject *obj)
{
    Q_D(QObject);
    if (!obj)
        return;
    if (d->threadData != obj->d_func()->threadData) {
        qWarning("QObject::installEventFilter(): Cannot filter events for objects in a different thread.");
        return;
    }

    if (!d->extraData)
        d->extraData = new QObjectPrivate::ExtraData;

    // clean up unused items in the list
    d->extraData->eventFilters.removeAll((QObject*)nullptr);
    d->extraData->eventFilters.removeAll(obj);
    d->extraData->eventFilters.prepend(obj);
}

/*!
    Removes an event filter object \a obj from this object. The
    request is ignored if such an event filter has not been installed.

    All event filters for this object are automatically removed when
    this object is destroyed.

    It is always safe to remove an event filter, even during event
    filter activation (i.e. from the eventFilter() function).

    \sa installEventFilter(), eventFilter(), event()
*/

void QObject::removeEventFilter(QObject *obj)
{
    Q_D(QObject);
    if (d->extraData) {
        for (int i = 0; i < d->extraData->eventFilters.count(); ++i) {
            if (d->extraData->eventFilters.at(i) == obj)
                d->extraData->eventFilters[i] = nullptr;
        }
    }
}


/*!
    \fn void QObject::destroyed(QObject *obj)

    This signal is emitted immediately before the object \a obj is
    destroyed, after any instances of QPointer have been notified,
    and cannot be blocked.

    All the objects's children are destroyed immediately after this
    signal is emitted.

    \sa deleteLater(), QPointer
*/

/*!
    \threadsafe

    Schedules this object for deletion.

    The object will be deleted when control returns to the event
    loop. If the event loop is not running when this function is
    called (e.g. deleteLater() is called on an object before
    QCoreApplication::exec()), the object will be deleted once the
    event loop is started. If deleteLater() is called after the main event loop
    has stopped, the object will not be deleted.
    Since Qt 4.8, if deleteLater() is called on an object that lives in a
    thread with no running event loop, the object will be destroyed when the
    thread finishes.

    Note that entering and leaving a new event loop (e.g., by opening a modal
    dialog) will \e not perform the deferred deletion; for the object to be
    deleted, the control must return to the event loop from which deleteLater()
    was called. This does not apply to objects deleted while a previous, nested
    event loop was still running: the Qt event loop will delete those objects
    as soon as the new nested event loop starts.

    \note It is safe to call this function more than once; when the
    first deferred deletion event is delivered, any pending events for the
    object are removed from the event queue.

    \sa destroyed(), QPointer
*/
void QObject::deleteLater()
{
#ifdef QT_DEBUG
    if (qApp == this)
        qWarning("You are deferring the delete of QCoreApplication, this may not work as expected.");
#endif
    QCoreApplication::postEvent(this, new QDeferredDeleteEvent());
}

/*!
    \fn QString QObject::tr(const char *sourceText, const char *disambiguation, int n)
    \reentrant

    Returns a translated version of \a sourceText, optionally based on a
    \a disambiguation string and value of \a n for strings containing plurals;
    otherwise returns QString::fromUtf8(\a sourceText) if no appropriate
    translated string is available.

    Example:
    \snippet ../widgets/mainwindows/sdi/mainwindow.cpp implicit tr context
    \dots

    If the same \a sourceText is used in different roles within the
    same context, an additional identifying string may be passed in
    \a disambiguation (\nullptr by default). In Qt 4.4 and earlier, this was
    the preferred way to pass comments to translators.

    Example:

    \snippet code/src_corelib_kernel_qobject.cpp 17
    \dots

    See \l{Writing Source Code for Translation} for a detailed description of
    Qt's translation mechanisms in general, and the
    \l{Writing Source Code for Translation#Disambiguation}{Disambiguation}
    section for information on disambiguation.

    \warning This method is reentrant only if all translators are
    installed \e before calling this method. Installing or removing
    translators while performing translations is not supported. Doing
    so will probably result in crashes or other undesirable behavior.

    \sa QCoreApplication::translate(), {Internationalization with Qt}
*/

/*!
    \fn QString QObject::trUtf8(const char *sourceText, const char *disambiguation, int n)
    \reentrant
    \obsolete

    Returns a translated version of \a sourceText, or
    QString::fromUtf8(\a sourceText) if there is no appropriate
    version. It is otherwise identical to tr(\a sourceText, \a
    disambiguation, \a n).

    \warning This method is reentrant only if all translators are
    installed \e before calling this method. Installing or removing
    translators while performing translations is not supported. Doing
    so will probably result in crashes or other undesirable behavior.

    \warning For portability reasons, we recommend that you use
    escape sequences for specifying non-ASCII characters in string
    literals to trUtf8(). For example:

    \snippet code/src_corelib_kernel_qobject.cpp 20

    \sa tr(), QCoreApplication::translate(), {Internationalization with Qt}
*/




/*****************************************************************************
  Signals and slots
 *****************************************************************************/


const char *qFlagLocation(const char *method)
{
    QThreadData *currentThreadData = QThreadData::current(false);
    if (currentThreadData != nullptr)
        currentThreadData->flaggedSignatures.store(method);
    return method;
}

static int extract_code(const char *member)
{
    // extract code, ensure QMETHOD_CODE <= code <= QSIGNAL_CODE
    return (((int)(*member) - '0') & 0x3);
}

static const char * extract_location(const char *member)
{
    if (QThreadData::current()->flaggedSignatures.contains(member)) {
        // signature includes location information after the first null-terminator
        const char *location = member + qstrlen(member) + 1;
        if (*location != '\0')
            return location;
    }
    return nullptr;
}

static bool check_signal_macro(const QObject *sender, const char *signal,
                                const char *func, const char *op)
{
    int sigcode = extract_code(signal);
    if (sigcode != QSIGNAL_CODE) {
        if (sigcode == QSLOT_CODE)
            qWarning("QObject::%s: Attempt to %s non-signal %s::%s",
                     func, op, sender->metaObject()->className(), signal+1);
        else
            qWarning("QObject::%s: Use the SIGNAL macro to %s %s::%s",
                     func, op, sender->metaObject()->className(), signal);
        return false;
    }
    return true;
}

static bool check_method_code(int code, const QObject *object,
                               const char *method, const char *func)
{
    if (code != QSLOT_CODE && code != QSIGNAL_CODE) {
        qWarning("QObject::%s: Use the SLOT or SIGNAL macro to "
                 "%s %s::%s", func, func, object->metaObject()->className(), method);
        return false;
    }
    return true;
}

Q_DECL_COLD_FUNCTION
static void err_method_notfound(const QObject *object,
                                const char *method, const char *func)
{
    const char *type = "method";
    switch (extract_code(method)) {
        case QSLOT_CODE:   type = "slot";   break;
        case QSIGNAL_CODE: type = "signal"; break;
    }
    const char *loc = extract_location(method);
    if (strchr(method,')') == nullptr)                // common typing mistake
        qWarning("QObject::%s: Parentheses expected, %s %s::%s%s%s",
                 func, type, object->metaObject()->className(), method+1,
                 loc ? " in ": "", loc ? loc : "");
    else
        qWarning("QObject::%s: No such %s %s::%s%s%s",
                 func, type, object->metaObject()->className(), method+1,
                 loc ? " in ": "", loc ? loc : "");

}


Q_DECL_COLD_FUNCTION
static void err_info_about_objects(const char * func,
                                    const QObject * sender,
                                    const QObject * receiver)
{
    QString a = sender ? sender->objectName() : QString();
    QString b = receiver ? receiver->objectName() : QString();
    if (!a.isEmpty())
        qWarning("QObject::%s:  (sender name:   '%s')", func, a.toLocal8Bit().data());
    if (!b.isEmpty())
        qWarning("QObject::%s:  (receiver name: '%s')", func, b.toLocal8Bit().data());
}

/*!
    Returns a pointer to the object that sent the signal, if called in
    a slot activated by a signal; otherwise it returns \nullptr. The pointer
    is valid only during the execution of the slot that calls this
    function from this object's thread context.

    The pointer returned by this function becomes invalid if the
    sender is destroyed, or if the slot is disconnected from the
    sender's signal.

    \warning This function violates the object-oriented principle of
    modularity. However, getting access to the sender might be useful
    when many signals are connected to a single slot.

    \warning As mentioned above, the return value of this function is
    not valid when the slot is called via a Qt::DirectConnection from
    a thread different from this object's thread. Do not use this
    function in this type of scenario.

    \sa senderSignalIndex()
*/

QObject *QObject::sender() const
{
    Q_D(const QObject);

    QBasicMutexLocker locker(signalSlotLock(this));
    QObjectPrivate::ConnectionData *cd = d->connections.loadRelaxed();
    if (!cd || !cd->currentSender)
        return nullptr;

    for (QObjectPrivate::Connection *c = cd->senders; c; c = c->next) {
        if (c->sender == cd->currentSender->sender)
            return cd->currentSender->sender;
    }

    return nullptr;
}

/*!
    \since 4.8

    Returns the meta-method index of the signal that called the currently
    executing slot, which is a member of the class returned by sender().
    If called outside of a slot activated by a signal, -1 is returned.

    For signals with default parameters, this function will always return
    the index with all parameters, regardless of which was used with
    connect(). For example, the signal \c {destroyed(QObject *obj = \nullptr)}
    will have two different indexes (with and without the parameter), but
    this function will always return the index with a parameter. This does
    not apply when overloading signals with different parameters.

    \warning This function violates the object-oriented principle of
    modularity. However, getting access to the signal index might be useful
    when many signals are connected to a single slot.

    \warning The return value of this function is not valid when the slot
    is called via a Qt::DirectConnection from a thread different from this
    object's thread. Do not use this function in this type of scenario.

    \sa sender(), QMetaObject::indexOfSignal(), QMetaObject::method()
*/

int QObject::senderSignalIndex() const
{
    Q_D(const QObject);

    QBasicMutexLocker locker(signalSlotLock(this));
    QObjectPrivate::ConnectionData *cd = d->connections.loadRelaxed();
    if (!cd || !cd->currentSender)
        return -1;

    for (QObjectPrivate::Connection *c = cd->senders; c; c = c->next) {
        if (c->sender == cd->currentSender->sender) {
            // Convert from signal range to method range
            return QMetaObjectPrivate::signal(c->sender->metaObject(), cd->currentSender->signal).methodIndex();
        }
    }

    return -1;
}

/*!
    Returns the number of receivers connected to the \a signal.

    Since both slots and signals can be used as receivers for signals,
    and the same connections can be made many times, the number of
    receivers is the same as the number of connections made from this
    signal.

    When calling this function, you can use the \c SIGNAL() macro to
    pass a specific signal:

    \snippet code/src_corelib_kernel_qobject.cpp 21

    \warning This function violates the object-oriented principle of
    modularity. However, it might be useful when you need to perform
    expensive initialization only if something is connected to a
    signal.

    \sa isSignalConnected()
*/

int QObject::receivers(const char *signal) const
{
    Q_D(const QObject);
    int receivers = 0;
    if (signal) {
        QByteArray signal_name = QMetaObject::normalizedSignature(signal);
        signal = signal_name;
#ifndef QT_NO_DEBUG
        if (!check_signal_macro(this, signal, "receivers", "bind"))
            return 0;
#endif
        signal++; // skip code
        int signal_index = d->signalIndex(signal);
        if (signal_index < 0) {
#ifndef QT_NO_DEBUG
            err_method_notfound(this, signal-1, "receivers");
#endif
            return 0;
        }

        if (!d->isSignalConnected(signal_index))
            return receivers;

        if (!d->isDeletingChildren && d->declarativeData && QAbstractDeclarativeData::receivers) {
            receivers += QAbstractDeclarativeData::receivers(d->declarativeData, this,
                                                             signal_index);
        }

        QObjectPrivate::ConnectionData *cd = d->connections.loadRelaxed();
        QBasicMutexLocker locker(signalSlotLock(this));
        if (cd && signal_index < cd->signalVectorCount()) {
            const QObjectPrivate::Connection *c = cd->signalVector.loadRelaxed()->at(signal_index).first.loadRelaxed();
            while (c) {
                receivers += c->receiver.loadRelaxed() ? 1 : 0;
                c = c->nextConnectionList.loadRelaxed();
            }
        }
    }
    return receivers;
}

/*!
    \since 5.0
    Returns \c true if the \a signal is connected to at least one receiver,
    otherwise returns \c false.

    \a signal must be a signal member of this object, otherwise the behaviour
    is undefined.

    \snippet code/src_corelib_kernel_qobject.cpp 49

    As the code snippet above illustrates, you can use this function
    to avoid emitting a signal that nobody listens to.

    \warning This function violates the object-oriented principle of
    modularity. However, it might be useful when you need to perform
    expensive initialization only if something is connected to a
    signal.
*/
bool QObject::isSignalConnected(const QMetaMethod &signal) const
{
    Q_D(const QObject);
    if (!signal.mobj)
        return false;

    Q_ASSERT_X(signal.mobj->cast(this) && signal.methodType() == QMetaMethod::Signal,
               "QObject::isSignalConnected" , "the parameter must be a signal member of the object");
    uint signalIndex = (signal.handle - QMetaObjectPrivate::get(signal.mobj)->methodData)/5;

    if (signal.mobj->d.data[signal.handle + 4] & MethodCloned)
        signalIndex = QMetaObjectPrivate::originalClone(signal.mobj, signalIndex);

    signalIndex += QMetaObjectPrivate::signalOffset(signal.mobj);

    QBasicMutexLocker locker(signalSlotLock(this));
    return d->isSignalConnected(signalIndex, true);
}

/*!
    \internal

    This helper function calculates signal and method index for the given
    member in the specified class.

    \list
    \li If member.mobj is \nullptr then both signalIndex and methodIndex are set to -1.

    \li If specified member is not a member of obj instance class (or one of
    its parent classes) then both signalIndex and methodIndex are set to -1.
    \endlist

    This function is used by QObject::connect and QObject::disconnect which
    are working with QMetaMethod.

    \a signalIndex is set to the signal index of member. If the member
    specified is not signal this variable is set to -1.

    \a methodIndex is set to the method index of the member. If the
    member is not a method of the object specified by the \a obj argument this
    variable is set to -1.
*/
void QMetaObjectPrivate::memberIndexes(const QObject *obj,
                                       const QMetaMethod &member,
                                       int *signalIndex, int *methodIndex)
{
    *signalIndex = -1;
    *methodIndex = -1;
    if (!obj || !member.mobj)
        return;
    const QMetaObject *m = obj->metaObject();
    // Check that member is member of obj class
    while (m != nullptr && m != member.mobj)
        m = m->d.superdata;
    if (!m)
        return;
    *signalIndex = *methodIndex = (member.handle - get(member.mobj)->methodData)/5;

    int signalOffset;
    int methodOffset;
    computeOffsets(m, &signalOffset, &methodOffset);

    *methodIndex += methodOffset;
    if (member.methodType() == QMetaMethod::Signal) {
        *signalIndex = originalClone(m, *signalIndex);
        *signalIndex += signalOffset;
    } else {
        *signalIndex = -1;
    }
}

#ifndef QT_NO_DEBUG
static inline void check_and_warn_compat(const QMetaObject *sender, const QMetaMethod &signal,
                                         const QMetaObject *receiver, const QMetaMethod &method)
{
    if (signal.attributes() & QMetaMethod::Compatibility) {
        if (!(method.attributes() & QMetaMethod::Compatibility))
            qWarning("QObject::connect: Connecting from COMPAT signal (%s::%s)",
                     sender->className(), signal.methodSignature().constData());
    } else if ((method.attributes() & QMetaMethod::Compatibility) &&
               method.methodType() == QMetaMethod::Signal) {
        qWarning("QObject::connect: Connecting from %s::%s to COMPAT slot (%s::%s)",
                 sender->className(), signal.methodSignature().constData(),
                 receiver->className(), method.methodSignature().constData());
    }
}
#endif

/*!
    \threadsafe

    Creates a connection of the given \a type from the \a signal in
    the \a sender object to the \a method in the \a receiver object.
    Returns a handle to the connection that can be used to disconnect
    it later.

    You must use the \c SIGNAL() and \c SLOT() macros when specifying
    the \a signal and the \a method, for example:

    \snippet code/src_corelib_kernel_qobject.cpp 22

    This example ensures that the label always displays the current
    scroll bar value. Note that the signal and slots parameters must not
    contain any variable names, only the type. E.g. the following would
    not work and return false:

    \snippet code/src_corelib_kernel_qobject.cpp 23

    A signal can also be connected to another signal:

    \snippet code/src_corelib_kernel_qobject.cpp 24

    In this example, the \c MyWidget constructor relays a signal from
    a private member variable, and makes it available under a name
    that relates to \c MyWidget.

    A signal can be connected to many slots and signals. Many signals
    can be connected to one slot.

    If a signal is connected to several slots, the slots are activated
    in the same order in which the connections were made, when the
    signal is emitted.

    The function returns a QMetaObject::Connection that represents
    a handle to a connection if it successfully
    connects the signal to the slot. The connection handle will be invalid
    if it cannot create the connection, for example, if QObject is unable
    to verify the existence of either \a signal or \a method, or if their
    signatures aren't compatible.
    You can check if the handle is valid by casting it to a bool.

    By default, a signal is emitted for every connection you make;
    two signals are emitted for duplicate connections. You can break
    all of these connections with a single disconnect() call.
    If you pass the Qt::UniqueConnection \a type, the connection will only
    be made if it is not a duplicate. If there is already a duplicate
    (exact same signal to the exact same slot on the same objects),
    the connection will fail and connect will return an invalid QMetaObject::Connection.

    \note Qt::UniqueConnections do not work for lambdas, non-member functions
    and functors; they only apply to connecting to member functions.

    The optional \a type parameter describes the type of connection
    to establish. In particular, it determines whether a particular
    signal is delivered to a slot immediately or queued for delivery
    at a later time. If the signal is queued, the parameters must be
    of types that are known to Qt's meta-object system, because Qt
    needs to copy the arguments to store them in an event behind the
    scenes. If you try to use a queued connection and get the error
    message

    \snippet code/src_corelib_kernel_qobject.cpp 25

    call qRegisterMetaType() to register the data type before you
    establish the connection.

    \sa disconnect(), sender(), qRegisterMetaType(), Q_DECLARE_METATYPE(),
    {Differences between String-Based and Functor-Based Connections}
*/
QMetaObject::Connection QObject::connect(const QObject *sender, const char *signal,
                                     const QObject *receiver, const char *method,
                                     Qt::ConnectionType type)
{
    if (sender == nullptr || receiver == nullptr || signal == nullptr || method == nullptr) {
        qWarning("QObject::connect: Cannot connect %s::%s to %s::%s",
                 sender ? sender->metaObject()->className() : "(nullptr)",
                 (signal && *signal) ? signal+1 : "(nullptr)",
                 receiver ? receiver->metaObject()->className() : "(nullptr)",
                 (method && *method) ? method+1 : "(nullptr)");
        return QMetaObject::Connection(nullptr);
    }
    QByteArray tmp_signal_name;

    if (!check_signal_macro(sender, signal, "connect", "bind"))
        return QMetaObject::Connection(nullptr);
    const QMetaObject *smeta = sender->metaObject();
    const char *signal_arg = signal;
    ++signal; //skip code
    QArgumentTypeArray signalTypes;
    Q_ASSERT(QMetaObjectPrivate::get(smeta)->revision >= 7);
    QByteArray signalName = QMetaObjectPrivate::decodeMethodSignature(signal, signalTypes);
    int signal_index = QMetaObjectPrivate::indexOfSignalRelative(
            &smeta, signalName, signalTypes.size(), signalTypes.constData());
    if (signal_index < 0) {
        // check for normalized signatures
        tmp_signal_name = QMetaObject::normalizedSignature(signal - 1);
        signal = tmp_signal_name.constData() + 1;

        signalTypes.clear();
        signalName = QMetaObjectPrivate::decodeMethodSignature(signal, signalTypes);
        smeta = sender->metaObject();
        signal_index = QMetaObjectPrivate::indexOfSignalRelative(
                &smeta, signalName, signalTypes.size(), signalTypes.constData());
    }
    if (signal_index < 0) {
        err_method_notfound(sender, signal_arg, "connect");
        err_info_about_objects("connect", sender, receiver);
        return QMetaObject::Connection(nullptr);
    }
    signal_index = QMetaObjectPrivate::originalClone(smeta, signal_index);
    signal_index += QMetaObjectPrivate::signalOffset(smeta);

    QByteArray tmp_method_name;
    int membcode = extract_code(method);

    if (!check_method_code(membcode, receiver, method, "connect"))
        return QMetaObject::Connection(nullptr);
    const char *method_arg = method;
    ++method; // skip code

    QArgumentTypeArray methodTypes;
    QByteArray methodName = QMetaObjectPrivate::decodeMethodSignature(method, methodTypes);
    const QMetaObject *rmeta = receiver->metaObject();
    int method_index_relative = -1;
    Q_ASSERT(QMetaObjectPrivate::get(rmeta)->revision >= 7);
    switch (membcode) {
    case QSLOT_CODE:
        method_index_relative = QMetaObjectPrivate::indexOfSlotRelative(
                &rmeta, methodName, methodTypes.size(), methodTypes.constData());
        break;
    case QSIGNAL_CODE:
        method_index_relative = QMetaObjectPrivate::indexOfSignalRelative(
                &rmeta, methodName, methodTypes.size(), methodTypes.constData());
        break;
    }
    if (method_index_relative < 0) {
        // check for normalized methods
        tmp_method_name = QMetaObject::normalizedSignature(method);
        method = tmp_method_name.constData();

        methodTypes.clear();
        methodName = QMetaObjectPrivate::decodeMethodSignature(method, methodTypes);
        // rmeta may have been modified above
        rmeta = receiver->metaObject();
        switch (membcode) {
        case QSLOT_CODE:
            method_index_relative = QMetaObjectPrivate::indexOfSlotRelative(
                    &rmeta, methodName, methodTypes.size(), methodTypes.constData());
            break;
        case QSIGNAL_CODE:
            method_index_relative = QMetaObjectPrivate::indexOfSignalRelative(
                    &rmeta, methodName, methodTypes.size(), methodTypes.constData());
            break;
        }
    }

    if (method_index_relative < 0) {
        err_method_notfound(receiver, method_arg, "connect");
        err_info_about_objects("connect", sender, receiver);
        return QMetaObject::Connection(nullptr);
    }

    if (!QMetaObjectPrivate::checkConnectArgs(signalTypes.size(), signalTypes.constData(),
                                              methodTypes.size(), methodTypes.constData())) {
        qWarning("QObject::connect: Incompatible sender/receiver arguments"
                 "\n        %s::%s --> %s::%s",
                 sender->metaObject()->className(), signal,
                 receiver->metaObject()->className(), method);
        return QMetaObject::Connection(nullptr);
    }

    int *types = nullptr;
    if ((type == Qt::QueuedConnection)
            && !(types = queuedConnectionTypes(signalTypes.constData(), signalTypes.size()))) {
        return QMetaObject::Connection(nullptr);
    }

#ifndef QT_NO_DEBUG
    QMetaMethod smethod = QMetaObjectPrivate::signal(smeta, signal_index);
    QMetaMethod rmethod = rmeta->method(method_index_relative + rmeta->methodOffset());
    check_and_warn_compat(smeta, smethod, rmeta, rmethod);
#endif
    QMetaObject::Connection handle = QMetaObject::Connection(QMetaObjectPrivate::connect(
        sender, signal_index, smeta, receiver, method_index_relative, rmeta ,type, types));
    return handle;
}

/*!
    \since 4.8

    Creates a connection of the given \a type from the \a signal in
    the \a sender object to the \a method in the \a receiver object.
    Returns a handle to the connection that can be used to disconnect
    it later.

    The Connection handle will be invalid  if it cannot create the
    connection, for example, the parameters were invalid.
    You can check if the QMetaObject::Connection is valid by casting it to a bool.

    This function works in the same way as
    \c {connect(const QObject *sender, const char *signal,
            const QObject *receiver, const char *method,
            Qt::ConnectionType type)}
    but it uses QMetaMethod to specify signal and method.

    \sa connect(const QObject *sender, const char *signal, const QObject *receiver, const char *method, Qt::ConnectionType type)
 */
QMetaObject::Connection QObject::connect(const QObject *sender, const QMetaMethod &signal,
                                     const QObject *receiver, const QMetaMethod &method,
                                     Qt::ConnectionType type)
{
    if (sender == nullptr
            || receiver == nullptr
            || signal.methodType() != QMetaMethod::Signal
            || method.methodType() == QMetaMethod::Constructor) {
        qWarning("QObject::connect: Cannot connect %s::%s to %s::%s",
                 sender ? sender->metaObject()->className() : "(nullptr)",
                 signal.methodSignature().constData(),
                 receiver ? receiver->metaObject()->className() : "(nullptr)",
                 method.methodSignature().constData() );
        return QMetaObject::Connection(nullptr);
    }

    int signal_index;
    int method_index;
    {
        int dummy;
        QMetaObjectPrivate::memberIndexes(sender, signal, &signal_index, &dummy);
        QMetaObjectPrivate::memberIndexes(receiver, method, &dummy, &method_index);
    }

    const QMetaObject *smeta = sender->metaObject();
    const QMetaObject *rmeta = receiver->metaObject();
    if (signal_index == -1) {
        qWarning("QObject::connect: Can't find signal %s on instance of class %s",
                 signal.methodSignature().constData(), smeta->className());
        return QMetaObject::Connection(nullptr);
    }
    if (method_index == -1) {
        qWarning("QObject::connect: Can't find method %s on instance of class %s",
                 method.methodSignature().constData(), rmeta->className());
        return QMetaObject::Connection(nullptr);
    }

    if (!QMetaObject::checkConnectArgs(signal.methodSignature().constData(), method.methodSignature().constData())) {
        qWarning("QObject::connect: Incompatible sender/receiver arguments"
                 "\n        %s::%s --> %s::%s",
                 smeta->className(), signal.methodSignature().constData(),
                 rmeta->className(), method.methodSignature().constData());
        return QMetaObject::Connection(nullptr);
    }

    int *types = nullptr;
    if ((type == Qt::QueuedConnection)
            && !(types = queuedConnectionTypes(signal.parameterTypes())))
        return QMetaObject::Connection(nullptr);

#ifndef QT_NO_DEBUG
    check_and_warn_compat(smeta, signal, rmeta, method);
#endif
    QMetaObject::Connection handle = QMetaObject::Connection(QMetaObjectPrivate::connect(
        sender, signal_index, signal.enclosingMetaObject(), receiver, method_index, nullptr, type, types));
    return handle;
}

/*!
    \fn bool QObject::connect(const QObject *sender, const char *signal, const char *method, Qt::ConnectionType type) const
    \overload connect()
    \threadsafe

    Connects \a signal from the \a sender object to this object's \a
    method.

    Equivalent to connect(\a sender, \a signal, \c this, \a method, \a type).

    Every connection you make emits a signal, so duplicate connections emit
    two signals. You can break a connection using disconnect().

    \sa disconnect()
*/

/*!
    \threadsafe

    Disconnects \a signal in object \a sender from \a method in object
    \a receiver. Returns \c true if the connection is successfully broken;
    otherwise returns \c false.

    A signal-slot connection is removed when either of the objects
    involved are destroyed.

    disconnect() is typically used in three ways, as the following
    examples demonstrate.
    \list 1
    \li Disconnect everything connected to an object's signals:

       \snippet code/src_corelib_kernel_qobject.cpp 26

       equivalent to the non-static overloaded function

       \snippet code/src_corelib_kernel_qobject.cpp 27

    \li Disconnect everything connected to a specific signal:

       \snippet code/src_corelib_kernel_qobject.cpp 28

       equivalent to the non-static overloaded function

       \snippet code/src_corelib_kernel_qobject.cpp 29

    \li Disconnect a specific receiver:

       \snippet code/src_corelib_kernel_qobject.cpp 30

       equivalent to the non-static overloaded function

       \snippet code/src_corelib_kernel_qobject.cpp 31

    \endlist

    \nullptr may be used as a wildcard, meaning "any signal", "any receiving
    object", or "any slot in the receiving object", respectively.

    The \a sender may never be \nullptr. (You cannot disconnect signals
    from more than one object in a single call.)

    If \a signal is \nullptr, it disconnects \a receiver and \a method from
    any signal. If not, only the specified signal is disconnected.

    If \a receiver is \nullptr, it disconnects anything connected to \a
    signal. If not, slots in objects other than \a receiver are not
    disconnected.

    If \a method is \nullptr, it disconnects anything that is connected to \a
    receiver. If not, only slots named \a method will be disconnected,
    and all other slots are left alone. The \a method must be \nullptr
    if \a receiver is left out, so you cannot disconnect a
    specifically-named slot on all objects.

    \sa connect()
*/
bool QObject::disconnect(const QObject *sender, const char *signal,
                         const QObject *receiver, const char *method)
{
    if (sender == nullptr || (receiver == nullptr && method != nullptr)) {
        qWarning("QObject::disconnect: Unexpected nullptr parameter");
        return false;
    }

    const char *signal_arg = signal;
    QByteArray signal_name;
    bool signal_found = false;
    if (signal) {
        QT_TRY {
            signal_name = QMetaObject::normalizedSignature(signal);
            signal = signal_name.constData();
        } QT_CATCH (const std::bad_alloc &) {
            // if the signal is already normalized, we can continue.
            if (sender->metaObject()->indexOfSignal(signal + 1) == -1)
                QT_RETHROW;
        }

        if (!check_signal_macro(sender, signal, "disconnect", "unbind"))
            return false;
        signal++; // skip code
    }

    QByteArray method_name;
    const char *method_arg = method;
    int membcode = -1;
    bool method_found = false;
    if (method) {
        QT_TRY {
            method_name = QMetaObject::normalizedSignature(method);
            method = method_name.constData();
        } QT_CATCH(const std::bad_alloc &) {
            // if the method is already normalized, we can continue.
            if (receiver->metaObject()->indexOfMethod(method + 1) == -1)
                QT_RETHROW;
        }

        membcode = extract_code(method);
        if (!check_method_code(membcode, receiver, method, "disconnect"))
            return false;
        method++; // skip code
    }

    /* We now iterate through all the sender's and receiver's meta
     * objects in order to also disconnect possibly shadowed signals
     * and slots with the same signature.
    */
    bool res = false;
    const QMetaObject *smeta = sender->metaObject();
    QByteArray signalName;
    QArgumentTypeArray signalTypes;
    Q_ASSERT(QMetaObjectPrivate::get(smeta)->revision >= 7);
    if (signal)
        signalName = QMetaObjectPrivate::decodeMethodSignature(signal, signalTypes);
    QByteArray methodName;
    QArgumentTypeArray methodTypes;
    Q_ASSERT(!receiver || QMetaObjectPrivate::get(receiver->metaObject())->revision >= 7);
    if (method)
        methodName = QMetaObjectPrivate::decodeMethodSignature(method, methodTypes);
    do {
        int signal_index = -1;
        if (signal) {
            signal_index = QMetaObjectPrivate::indexOfSignalRelative(
                        &smeta, signalName, signalTypes.size(), signalTypes.constData());
            if (signal_index < 0)
                break;
            signal_index = QMetaObjectPrivate::originalClone(smeta, signal_index);
            signal_index += QMetaObjectPrivate::signalOffset(smeta);
            signal_found = true;
        }

        if (!method) {
            res |= QMetaObjectPrivate::disconnect(sender, signal_index, smeta, receiver, -1, nullptr);
        } else {
            const QMetaObject *rmeta = receiver->metaObject();
            do {
                int method_index = QMetaObjectPrivate::indexOfMethod(
                            rmeta, methodName, methodTypes.size(), methodTypes.constData());
                if (method_index >= 0)
                    while (method_index < rmeta->methodOffset())
                            rmeta = rmeta->superClass();
                if (method_index < 0)
                    break;
                res |= QMetaObjectPrivate::disconnect(sender, signal_index, smeta, receiver, method_index, nullptr);
                method_found = true;
            } while ((rmeta = rmeta->superClass()));
        }
    } while (signal && (smeta = smeta->superClass()));

    if (signal && !signal_found) {
        err_method_notfound(sender, signal_arg, "disconnect");
        err_info_about_objects("disconnect", sender, receiver);
    } else if (method && !method_found) {
        err_method_notfound(receiver, method_arg, "disconnect");
        err_info_about_objects("disconnect", sender, receiver);
    }
    if (res) {
        if (!signal)
            const_cast<QObject*>(sender)->disconnectNotify(QMetaMethod());
    }
    return res;
}

/*!
    \since 4.8

    Disconnects \a signal in object \a sender from \a method in object
    \a receiver. Returns \c true if the connection is successfully broken;
    otherwise returns \c false.

    This function provides the same possibilities like
    \c {disconnect(const QObject *sender, const char *signal, const QObject *receiver, const char *method) }
    but uses QMetaMethod to represent the signal and the method to be disconnected.

    Additionally this function returns false and no signals and slots disconnected
    if:
    \list 1

        \li \a signal is not a member of sender class or one of its parent classes.

        \li \a method is not a member of receiver class or one of its parent classes.

        \li \a signal instance represents not a signal.

    \endlist

    QMetaMethod() may be used as wildcard in the meaning "any signal" or "any slot in receiving object".
    In the same way \nullptr can be used for \a receiver in the meaning "any receiving object".
    In this case method should also be QMetaMethod(). \a sender parameter should be never \nullptr.

    \sa disconnect(const QObject *sender, const char *signal, const QObject *receiver, const char *method)
 */
bool QObject::disconnect(const QObject *sender, const QMetaMethod &signal,
                         const QObject *receiver, const QMetaMethod &method)
{
    if (sender == nullptr || (receiver == nullptr && method.mobj != nullptr)) {
        qWarning("QObject::disconnect: Unexpected nullptr parameter");
        return false;
    }
    if (signal.mobj) {
        if(signal.methodType() != QMetaMethod::Signal) {
            qWarning("QObject::%s: Attempt to %s non-signal %s::%s",
                     "disconnect","unbind",
                     sender->metaObject()->className(), signal.methodSignature().constData());
            return false;
        }
    }
    if (method.mobj) {
        if(method.methodType() == QMetaMethod::Constructor) {
            qWarning("QObject::disconnect: cannot use constructor as argument %s::%s",
                     receiver->metaObject()->className(), method.methodSignature().constData());
            return false;
        }
    }

    // Reconstructing SIGNAL() macro result for signal.methodSignature() string
    QByteArray signalSignature;
    if (signal.mobj) {
        signalSignature.reserve(signal.methodSignature().size()+1);
        signalSignature.append((char)(QSIGNAL_CODE + '0'));
        signalSignature.append(signal.methodSignature());
    }

    int signal_index;
    int method_index;
    {
        int dummy;
        QMetaObjectPrivate::memberIndexes(sender, signal, &signal_index, &dummy);
        QMetaObjectPrivate::memberIndexes(receiver, method, &dummy, &method_index);
    }
    // If we are here sender is not nullptr. If signal is not nullptr while signal_index
    // is -1 then this signal is not a member of sender.
    if (signal.mobj && signal_index == -1) {
        qWarning("QObject::disconnect: signal %s not found on class %s",
                 signal.methodSignature().constData(), sender->metaObject()->className());
        return false;
    }
    // If this condition is true then method is not a member of receiver.
    if (receiver && method.mobj && method_index == -1) {
        qWarning("QObject::disconnect: method %s not found on class %s",
                 method.methodSignature().constData(), receiver->metaObject()->className());
        return false;
    }

    if (!QMetaObjectPrivate::disconnect(sender, signal_index, signal.mobj, receiver, method_index, nullptr))
        return false;

    if (!signal.isValid()) {
        // The signal is a wildcard, meaning all signals were disconnected.
        // QMetaObjectPrivate::disconnect() doesn't call disconnectNotify()
        // per connection in this case. Call it once now, with an invalid
        // QMetaMethod as argument, as documented.
        const_cast<QObject*>(sender)->disconnectNotify(signal);
    }
    return true;
}

/*!
    \threadsafe

    \fn bool QObject::disconnect(const char *signal, const QObject *receiver, const char *method) const
    \overload disconnect()

    Disconnects \a signal from \a method of \a receiver.

    A signal-slot connection is removed when either of the objects
    involved are destroyed.
*/

/*!
    \fn bool QObject::disconnect(const QObject *receiver, const char *method) const
    \overload disconnect()

    Disconnects all signals in this object from \a receiver's \a
    method.

    A signal-slot connection is removed when either of the objects
    involved are destroyed.
*/


/*!
    \since 5.0

    This virtual function is called when something has been connected
    to \a signal in this object.

    If you want to compare \a signal with a specific signal, you can
    use QMetaMethod::fromSignal() as follows:

    \snippet code/src_corelib_kernel_qobject.cpp 32

    \warning This function violates the object-oriented principle of
    modularity. However, it might be useful when you need to perform
    expensive initialization only if something is connected to a
    signal.

    \warning This function is called from the thread which performs the
    connection, which may be a different thread from the thread in
    which this object lives.

    \sa connect(), disconnectNotify()
*/

void QObject::connectNotify(const QMetaMethod &signal)
{
    Q_UNUSED(signal);
}

/*!
    \since 5.0

    This virtual function is called when something has been
    disconnected from \a signal in this object.

    See connectNotify() for an example of how to compare
    \a signal with a specific signal.

    If all signals were disconnected from this object (e.g., the
    signal argument to disconnect() was \nullptr), disconnectNotify()
    is only called once, and the \a signal will be an invalid
    QMetaMethod (QMetaMethod::isValid() returns \c false).

    \warning This function violates the object-oriented principle of
    modularity. However, it might be useful for optimizing access to
    expensive resources.

    \warning This function is called from the thread which performs the
    disconnection, which may be a different thread from the thread in
    which this object lives. This function may also be called with a QObject
    internal mutex locked. It is therefore not allowed to re-enter any
    of any QObject functions from your reimplementation and if you lock
    a mutex in your reimplementation, make sure that you don't call QObject
    functions with that mutex held in other places or it will result in
    a deadlock.

    \sa disconnect(), connectNotify()
*/

void QObject::disconnectNotify(const QMetaMethod &signal)
{
    Q_UNUSED(signal);
}

/*
    \internal
    convert a signal index from the method range to the signal range
 */
static int methodIndexToSignalIndex(const QMetaObject **base, int signal_index)
{
    if (signal_index < 0)
        return signal_index;
    const QMetaObject *metaObject = *base;
    while (metaObject && metaObject->methodOffset() > signal_index)
        metaObject = metaObject->superClass();

    if (metaObject) {
        int signalOffset, methodOffset;
        computeOffsets(metaObject, &signalOffset, &methodOffset);
        if (signal_index < metaObject->methodCount())
            signal_index = QMetaObjectPrivate::originalClone(metaObject, signal_index - methodOffset) + signalOffset;
        else
            signal_index = signal_index - methodOffset + signalOffset;
        *base = metaObject;
    }
    return signal_index;
}

/*!
   \internal
   \a types is a 0-terminated vector of meta types for queued
   connections.

   if \a signal_index is -1, then we effectively connect *all* signals
   from the sender to the receiver's slot
 */
QMetaObject::Connection QMetaObject::connect(const QObject *sender, int signal_index,
                                          const QObject *receiver, int method_index, int type, int *types)
{
    const QMetaObject *smeta = sender->metaObject();
    signal_index = methodIndexToSignalIndex(&smeta, signal_index);
    return Connection(QMetaObjectPrivate::connect(sender, signal_index, smeta,
                                       receiver, method_index,
                                       nullptr, //FIXME, we could speed this connection up by computing the relative index
                                       type, types));
}

/*!
    \internal
   Same as the QMetaObject::connect, but \a signal_index must be the result of QObjectPrivate::signalIndex

    method_index is relative to the rmeta metaobject, if rmeta is \nullptr, then it is absolute index

    the QObjectPrivate::Connection* has a refcount of 2, so it must be passed to a QMetaObject::Connection
 */
QObjectPrivate::Connection *QMetaObjectPrivate::connect(const QObject *sender,
                                 int signal_index, const QMetaObject *smeta,
                                 const QObject *receiver, int method_index,
                                 const QMetaObject *rmeta, int type, int *types)
{
    QObject *s = const_cast<QObject *>(sender);
    QObject *r = const_cast<QObject *>(receiver);

    int method_offset = rmeta ? rmeta->methodOffset() : 0;
    Q_ASSERT(!rmeta || QMetaObjectPrivate::get(rmeta)->revision >= 6);
    QObjectPrivate::StaticMetaCallFunction callFunction = rmeta ? rmeta->d.static_metacall : nullptr;

    QOrderedMutexLocker locker(signalSlotLock(sender),
                               signalSlotLock(receiver));

    QObjectPrivate::ConnectionData *scd  = QObjectPrivate::get(s)->connections.loadRelaxed();
    if (type & Qt::UniqueConnection && scd) {
        if (scd->signalVectorCount() > signal_index) {
            const QObjectPrivate::Connection *c2 = scd->signalVector.loadRelaxed()->at(signal_index).first.loadRelaxed();

            int method_index_absolute = method_index + method_offset;

            while (c2) {
                if (!c2->isSlotObject && c2->receiver.loadRelaxed() == receiver && c2->method() == method_index_absolute)
                    return nullptr;
                c2 = c2->nextConnectionList.loadRelaxed();
            }
        }
        type &= Qt::UniqueConnection - 1;
    }

    std::unique_ptr<QObjectPrivate::Connection> c{new QObjectPrivate::Connection};
    c->sender = s;
    c->signal_index = signal_index;
    c->receiver.storeRelaxed(r);
    QThreadData *td = r->d_func()->threadData;
    td->ref();
    c->receiverThreadData.storeRelaxed(td);
    c->method_relative = method_index;
    c->method_offset = method_offset;
    c->connectionType = type;
    c->isSlotObject = false;
    c->argumentTypes.storeRelaxed(types);
    c->callFunction = callFunction;

    QObjectPrivate::get(s)->addConnection(signal_index, c.get());

    locker.unlock();
    QMetaMethod smethod = QMetaObjectPrivate::signal(smeta, signal_index);
    if (smethod.isValid())
        s->connectNotify(smethod);

    return c.release();
}

/*!
    \internal
 */
bool QMetaObject::disconnect(const QObject *sender, int signal_index,
                             const QObject *receiver, int method_index)
{
    const QMetaObject *smeta = sender->metaObject();
    signal_index = methodIndexToSignalIndex(&smeta, signal_index);
    return QMetaObjectPrivate::disconnect(sender, signal_index, smeta,
                                          receiver, method_index, nullptr);
}

/*!
    \internal

Disconnect a single signal connection.  If QMetaObject::connect() has been called
multiple times for the same sender, signal_index, receiver and method_index only
one of these connections will be removed.
 */
bool QMetaObject::disconnectOne(const QObject *sender, int signal_index,
                                const QObject *receiver, int method_index)
{
    const QMetaObject *smeta = sender->metaObject();
    signal_index = methodIndexToSignalIndex(&smeta, signal_index);
    return QMetaObjectPrivate::disconnect(sender, signal_index, smeta,
                                          receiver, method_index, nullptr,
                                          QMetaObjectPrivate::DisconnectOne);
}

/*!
    \internal
    Helper function to remove the connection from the senders list and set the receivers to \nullptr
 */
bool QMetaObjectPrivate::disconnectHelper(QObjectPrivate::ConnectionData *connections, int signalIndex,
                                          const QObject *receiver, int method_index, void **slot,
                                          QBasicMutex *senderMutex, DisconnectType disconnectType)
{
    bool success = false;

    auto &connectionList = connections->connectionsForSignal(signalIndex);
    auto *c = connectionList.first.loadRelaxed();
    while (c) {
        QObject *r = c->receiver.loadRelaxed();
        if (r && (receiver == nullptr || (r == receiver
                           && (method_index < 0 || (!c->isSlotObject && c->method() == method_index))
                           && (slot == nullptr || (c->isSlotObject && c->slotObj->compare(slot)))))) {
            bool needToUnlock = false;
            QBasicMutex *receiverMutex = nullptr;
            if (r) {
                receiverMutex = signalSlotLock(r);
                // need to relock this receiver and sender in the correct order
                needToUnlock = QOrderedMutexLocker::relock(senderMutex, receiverMutex);
            }
            if (c->receiver.loadRelaxed())
                connections->removeConnection(c);

            if (needToUnlock)
                receiverMutex->unlock();

            success = true;

            if (disconnectType == DisconnectOne)
                return success;
        }
        c = c->nextConnectionList.loadRelaxed();
    }
    return success;
}

/*!
    \internal
    Same as the QMetaObject::disconnect, but \a signal_index must be the result of QObjectPrivate::signalIndex
 */
bool QMetaObjectPrivate::disconnect(const QObject *sender,
                                    int signal_index, const QMetaObject *smeta,
                                    const QObject *receiver, int method_index, void **slot,
                                    DisconnectType disconnectType)
{
    if (!sender)
        return false;

    QObject *s = const_cast<QObject *>(sender);

    QBasicMutex *senderMutex = signalSlotLock(sender);
    QBasicMutexLocker locker(senderMutex);

    QObjectPrivate::ConnectionData *scd  = QObjectPrivate::get(s)->connections.loadRelaxed();
    if (!scd)
        return false;

    bool success = false;
    {
        // prevent incoming connections changing the connections->receivers while unlocked
        QObjectPrivate::ConnectionDataPointer connections(scd);

        if (signal_index < 0) {
            // remove from all connection lists
            for (int sig_index = -1; sig_index < scd->signalVectorCount(); ++sig_index) {
                if (disconnectHelper(connections.data(), sig_index, receiver, method_index, slot, senderMutex, disconnectType))
                    success = true;
            }
        } else if (signal_index < scd->signalVectorCount()) {
            if (disconnectHelper(connections.data(), signal_index, receiver, method_index, slot, senderMutex, disconnectType))
                success = true;
        }
    }

    locker.unlock();
    if (success) {
        scd->cleanOrphanedConnections(s);

        QMetaMethod smethod = QMetaObjectPrivate::signal(smeta, signal_index);
        if (smethod.isValid())
            s->disconnectNotify(smethod);
    }

    return success;
}

// Helpers for formatting the connect statements of connectSlotsByName()'s debug mode
static QByteArray formatConnectionSignature(const char *className, const QMetaMethod &method)
{
    const auto signature = method.methodSignature();
    Q_ASSERT(signature.endsWith(')'));
    const int openParen = signature.indexOf('(');
    const bool hasParameters = openParen >= 0 && openParen < signature.size() - 2;
    QByteArray result;
    if (hasParameters) {
        result += "qOverload<"
            + signature.mid(openParen + 1, signature.size() - openParen - 2) + ">(";
    }
    result += '&';
    result += className + QByteArrayLiteral("::") + method.name();
    if (hasParameters)
        result += ')';
    return result;
}

static QByteArray msgConnect(const QMetaObject *senderMo, const QByteArray &senderName,
                             const QMetaMethod &signal, const QObject *receiver, int receiverIndex)
{
    const auto receiverMo = receiver->metaObject();
    const auto slot = receiverMo->method(receiverIndex);
    QByteArray message = QByteArrayLiteral("QObject::connect(")
        + senderName + ", " + formatConnectionSignature(senderMo->className(), signal)
        + ", " + receiver->objectName().toLatin1() + ", "
        + formatConnectionSignature(receiverMo->className(), slot) + ");";
    return message;
}

/*!
    \fn void QMetaObject::connectSlotsByName(QObject *object)

    Searches recursively for all child objects of the given \a object, and connects
    matching signals from them to slots of \a object that follow the following form:

    \snippet code/src_corelib_kernel_qobject.cpp 33

    Let's assume our object has a child object of type \c{QPushButton} with
    the \l{QObject::objectName}{object name} \c{button1}. The slot to catch the
    button's \c{clicked()} signal would be:

    \snippet code/src_corelib_kernel_qobject.cpp 34

    If \a object itself has a properly set object name, its own signals are also
    connected to its respective slots.

    \sa QObject::setObjectName()
 */
void QMetaObject::connectSlotsByName(QObject *o)
{
    if (!o)
        return;
    const QMetaObject *mo = o->metaObject();
    Q_ASSERT(mo);
    const QObjectList list = // list of all objects to look for matching signals including...
            o->findChildren<QObject *>(QString()) // all children of 'o'...
            << o; // and the object 'o' itself

    // for each method/slot of o ...
    for (int i = 0; i < mo->methodCount(); ++i) {
        const QByteArray slotSignature = mo->method(i).methodSignature();
        const char *slot = slotSignature.constData();
        Q_ASSERT(slot);

        // ...that starts with "on_", ...
        if (slot[0] != 'o' || slot[1] != 'n' || slot[2] != '_')
            continue;

        // ...we check each object in our list, ...
        bool foundIt = false;
        for(int j = 0; j < list.count(); ++j) {
            const QObject *co = list.at(j);
            const QByteArray coName = co->objectName().toLatin1();

            // ...discarding those whose objectName is not fitting the pattern "on_<objectName>_...", ...
            if (coName.isEmpty() || qstrncmp(slot + 3, coName.constData(), coName.size()) || slot[coName.size()+3] != '_')
                continue;

            const char *signal = slot + coName.size() + 4; // the 'signal' part of the slot name

            // ...for the presence of a matching signal "on_<objectName>_<signal>".
            const QMetaObject *smeta;
            int sigIndex = co->d_func()->signalIndex(signal, &smeta);
            if (sigIndex < 0) {
                // if no exactly fitting signal (name + complete parameter type list) could be found
                // look for just any signal with the correct name and at least the slot's parameter list.
                // Note: if more than one of those signals exist, the one that gets connected is
                // chosen 'at random' (order of declaration in source file)
                QList<QByteArray> compatibleSignals;
                const QMetaObject *smo = co->metaObject();
                int sigLen = qstrlen(signal) - 1; // ignore the trailing ')'
                for (int k = QMetaObjectPrivate::absoluteSignalCount(smo)-1; k >= 0; --k) {
                    const QMetaMethod method = QMetaObjectPrivate::signal(smo, k);
                    if (!qstrncmp(method.methodSignature().constData(), signal, sigLen)) {
                        smeta = method.enclosingMetaObject();
                        sigIndex = k;
                        compatibleSignals.prepend(method.methodSignature());
                    }
                }
                if (compatibleSignals.size() > 1)
                    qWarning() << "QMetaObject::connectSlotsByName: Connecting slot" << slot
                               << "with the first of the following compatible signals:" << compatibleSignals;
            }

            if (sigIndex < 0)
                continue;

            // we connect it...
            if (Connection(QMetaObjectPrivate::connect(co, sigIndex, smeta, o, i))) {
                foundIt = true;
                qCDebug(lcConnections, "%s",
                        msgConnect(smeta, coName, QMetaObjectPrivate::signal(smeta, sigIndex), o,  i).constData());
                // ...and stop looking for further objects with the same name.
                // Note: the Designer will make sure each object name is unique in the above
                // 'list' but other code may create two child objects with the same name. In
                // this case one is chosen 'at random'.
                break;
            }
        }
        if (foundIt) {
            // we found our slot, now skip all overloads
            while (mo->method(i + 1).attributes() & QMetaMethod::Cloned)
                  ++i;
        } else if (!(mo->method(i).attributes() & QMetaMethod::Cloned)) {
            // check if the slot has the following signature: "on_..._...(..."
            int iParen = slotSignature.indexOf('(');
            int iLastUnderscore = slotSignature.lastIndexOf('_', iParen-1);
            if (iLastUnderscore > 3)
                qWarning("QMetaObject::connectSlotsByName: No matching signal for %s", slot);
        }
    }
}

/*!
    \internal

    \a signal must be in the signal index range (see QObjectPrivate::signalIndex()).
*/
static void queued_activate(QObject *sender, int signal, QObjectPrivate::Connection *c, void **argv)
{
    const int *argumentTypes = c->argumentTypes.loadRelaxed();
    if (!argumentTypes) {
        QMetaMethod m = QMetaObjectPrivate::signal(sender->metaObject(), signal);
        argumentTypes = queuedConnectionTypes(m.parameterTypes());
        if (!argumentTypes) // cannot queue arguments
            argumentTypes = &DIRECT_CONNECTION_ONLY;
        if (!c->argumentTypes.testAndSetOrdered(nullptr, argumentTypes)) {
            if (argumentTypes != &DIRECT_CONNECTION_ONLY)
                delete [] argumentTypes;
            argumentTypes = c->argumentTypes.loadRelaxed();
        }
    }
    if (argumentTypes == &DIRECT_CONNECTION_ONLY) // cannot activate
        return;
    int nargs = 1; // include return type
    while (argumentTypes[nargs-1])
        ++nargs;

    QBasicMutexLocker locker(signalSlotLock(c->receiver.loadRelaxed()));
    if (!c->receiver.loadRelaxed()) {
        // the connection has been disconnected before we got the lock
        return;
    }
    if (c->isSlotObject)
        c->slotObj->ref();
    locker.unlock();

    QMetaCallEvent *ev = c->isSlotObject ?
        new QMetaCallEvent(c->slotObj, sender, signal, nargs) :
        new QMetaCallEvent(c->method_offset, c->method_relative, c->callFunction, sender, signal, nargs);

    void **args = ev->args();
    int *types = ev->types();

    types[0] = 0; // return type
    args[0] = nullptr; // return value

    if (nargs > 1) {
        for (int n = 1; n < nargs; ++n)
            types[n] = argumentTypes[n-1];

        for (int n = 1; n < nargs; ++n)
            args[n] = QMetaType::create(types[n], argv[n]);
    }

    locker.relock();
    if (c->isSlotObject)
        c->slotObj->destroyIfLastRef();
    if (!c->receiver.loadRelaxed()) {
        // the connection has been disconnected while we were unlocked
        locker.unlock();
        delete ev;
        return;
    }

    QCoreApplication::postEvent(c->receiver.loadRelaxed(), ev);
}

template <bool callbacks_enabled>
void doActivate(QObject *sender, int signal_index, void **argv)
{
    QObjectPrivate *sp = QObjectPrivate::get(sender);

    if (sp->blockSig)
        return;

    Q_TRACE_SCOPE(QMetaObject_activate, sender, signal_index);

    if (sp->isDeclarativeSignalConnected(signal_index)
            && QAbstractDeclarativeData::signalEmitted) {
        Q_TRACE_SCOPE(QMetaObject_activate_declarative_signal, sender, signal_index);
        QAbstractDeclarativeData::signalEmitted(sp->declarativeData, sender,
                                                signal_index, argv);
    }

    const QSignalSpyCallbackSet *signal_spy_set = callbacks_enabled ? qt_signal_spy_callback_set.loadAcquire() : nullptr;

    void *empty_argv[] = { nullptr };
    if (!argv)
        argv = empty_argv;

    if (!sp->maybeSignalConnected(signal_index)) {
        // The possible declarative connection is done, and nothing else is connected
        if (callbacks_enabled && signal_spy_set->signal_begin_callback != nullptr)
            signal_spy_set->signal_begin_callback(sender, signal_index, argv);
        if (callbacks_enabled && signal_spy_set->signal_end_callback != nullptr)
            signal_spy_set->signal_end_callback(sender, signal_index);
        return;
    }

    if (callbacks_enabled && signal_spy_set->signal_begin_callback != nullptr)
        signal_spy_set->signal_begin_callback(sender, signal_index, argv);

    bool senderDeleted = false;
    {
    Q_ASSERT(sp->connections.loadAcquire());
    QObjectPrivate::ConnectionDataPointer connections(sp->connections.loadRelaxed());
    QObjectPrivate::SignalVector *signalVector = connections->signalVector.loadRelaxed();

    const QObjectPrivate::ConnectionList *list;
    if (signal_index < signalVector->count())
        list = &signalVector->at(signal_index);
    else
        list = &signalVector->at(-1);

    Qt::HANDLE currentThreadId = QThread::currentThreadId();
    bool inSenderThread = currentThreadId == QObjectPrivate::get(sender)->threadData.loadRelaxed()->threadId.loadRelaxed();

    // We need to check against the highest connection id to ensure that signals added
    // during the signal emission are not emitted in this emission.
    uint highestConnectionId = connections->currentConnectionId.loadRelaxed();
    do {
        QObjectPrivate::Connection *c = list->first.loadRelaxed();
        if (!c)
            continue;

        do {
            QObject * const receiver = c->receiver.loadRelaxed();
            if (!receiver)
                continue;

            QThreadData *td = c->receiverThreadData.loadRelaxed();
            if (!td)
                continue;

            bool receiverInSameThread;
            if (inSenderThread) {
                receiverInSameThread = currentThreadId == td->threadId.loadRelaxed();
            } else {
                // need to lock before reading the threadId, because moveToThread() could interfere
                QMutexLocker lock(signalSlotLock(receiver));
                receiverInSameThread = currentThreadId == td->threadId.loadRelaxed();
            }


            // determine if this connection should be sent immediately or
            // put into the event queue
            if ((c->connectionType == Qt::AutoConnection && !receiverInSameThread)
                || (c->connectionType == Qt::QueuedConnection)) {
                queued_activate(sender, signal_index, c, argv);
                continue;
#if QT_CONFIG(thread)
            } else if (c->connectionType == Qt::BlockingQueuedConnection) {
                if (receiverInSameThread) {
                    qWarning("Qt: Dead lock detected while activating a BlockingQueuedConnection: "
                    "Sender is %s(%p), receiver is %s(%p)",
                    sender->metaObject()->className(), sender,
                    receiver->metaObject()->className(), receiver);
                }
                QSemaphore semaphore;
                {
                    QBasicMutexLocker locker(signalSlotLock(sender));
                    if (!c->receiver.loadAcquire())
                        continue;
                    QMetaCallEvent *ev = c->isSlotObject ?
                        new QMetaCallEvent(c->slotObj, sender, signal_index, argv, &semaphore) :
                        new QMetaCallEvent(c->method_offset, c->method_relative, c->callFunction,
                                           sender, signal_index, argv, &semaphore);
                    QCoreApplication::postEvent(receiver, ev);
                }
                semaphore.acquire();
                continue;
#endif
            }

            QObjectPrivate::Sender senderData(receiverInSameThread ? receiver : nullptr, sender, signal_index);

            if (c->isSlotObject) {
                c->slotObj->ref();

                struct Deleter {
                    void operator()(QtPrivate::QSlotObjectBase *slot) const {
                        if (slot) slot->destroyIfLastRef();
                    }
                };
                const std::unique_ptr<QtPrivate::QSlotObjectBase, Deleter> obj{c->slotObj};

                {
                    Q_TRACE_SCOPE(QMetaObject_activate_slot_functor, obj.get());
                    obj->call(receiver, argv);
                }
            } else if (c->callFunction && c->method_offset <= receiver->metaObject()->methodOffset()) {
                //we compare the vtable to make sure we are not in the destructor of the object.
                const int method_relative = c->method_relative;
                const auto callFunction = c->callFunction;
                const int methodIndex = (Q_HAS_TRACEPOINTS || callbacks_enabled) ? c->method() : 0;
                if (callbacks_enabled && signal_spy_set->slot_begin_callback != nullptr)
                    signal_spy_set->slot_begin_callback(receiver, methodIndex, argv);

                {
                    Q_TRACE_SCOPE(QMetaObject_activate_slot, receiver, methodIndex);
                    callFunction(receiver, QMetaObject::InvokeMetaMethod, method_relative, argv);
                }

                if (callbacks_enabled && signal_spy_set->slot_end_callback != nullptr)
                    signal_spy_set->slot_end_callback(receiver, methodIndex);
            } else {
                const int method = c->method_relative + c->method_offset;

                if (callbacks_enabled && signal_spy_set->slot_begin_callback != nullptr) {
                    signal_spy_set->slot_begin_callback(receiver, method, argv);
                }

                {
                    Q_TRACE_SCOPE(QMetaObject_activate_slot, receiver, method);
                    QMetaObject::metacall(receiver, QMetaObject::InvokeMetaMethod, method, argv);
                }

                if (callbacks_enabled && signal_spy_set->slot_end_callback != nullptr)
                    signal_spy_set->slot_end_callback(receiver, method);
            }
        } while ((c = c->nextConnectionList.loadRelaxed()) != nullptr && c->id <= highestConnectionId);

    } while (list != &signalVector->at(-1) &&
        //start over for all signals;
        ((list = &signalVector->at(-1)), true));

        if (connections->currentConnectionId.loadRelaxed() == 0)
            senderDeleted = true;
    }
    if (!senderDeleted) {
        sp->connections.loadRelaxed()->cleanOrphanedConnections(sender);

        if (callbacks_enabled && signal_spy_set->signal_end_callback != nullptr)
            signal_spy_set->signal_end_callback(sender, signal_index);
    }
}

/*!
    \internal
 */
void QMetaObject::activate(QObject *sender, const QMetaObject *m, int local_signal_index,
                           void **argv)
{
    int signal_index = local_signal_index + QMetaObjectPrivate::signalOffset(m);

    if (Q_UNLIKELY(qt_signal_spy_callback_set.loadRelaxed()))
        doActivate<true>(sender, signal_index, argv);
    else
        doActivate<false>(sender, signal_index, argv);
}

/*!
    \internal
 */
void QMetaObject::activate(QObject *sender, int signalOffset, int local_signal_index, void **argv)
{
    int signal_index = signalOffset + local_signal_index;

    if (Q_UNLIKELY(qt_signal_spy_callback_set.loadRelaxed()))
        doActivate<true>(sender, signal_index, argv);
    else
        doActivate<false>(sender, signal_index, argv);
 }

/*!
    \internal
   signal_index comes from indexOfMethod()
*/
void QMetaObject::activate(QObject *sender, int signal_index, void **argv)
{
    const QMetaObject *mo = sender->metaObject();
    while (mo->methodOffset() > signal_index)
        mo = mo->superClass();
    activate(sender, mo, signal_index - mo->methodOffset(), argv);
}

/*!
    \internal
    Returns the signal index used in the internal connections->receivers vector.

    It is different from QMetaObject::indexOfSignal():  indexOfSignal is the same as indexOfMethod
    while QObjectPrivate::signalIndex is smaller because it doesn't give index to slots.

    If \a meta is not \nullptr, it is set to the meta-object where the signal was found.
*/
int QObjectPrivate::signalIndex(const char *signalName,
                                const QMetaObject **meta) const
{
    Q_Q(const QObject);
    const QMetaObject *base = q->metaObject();
    Q_ASSERT(QMetaObjectPrivate::get(base)->revision >= 7);
    QArgumentTypeArray types;
    QByteArray name = QMetaObjectPrivate::decodeMethodSignature(signalName, types);
    int relative_index = QMetaObjectPrivate::indexOfSignalRelative(
            &base, name, types.size(), types.constData());
    if (relative_index < 0)
        return relative_index;
    relative_index = QMetaObjectPrivate::originalClone(base, relative_index);
    if (meta)
        *meta = base;
    return relative_index + QMetaObjectPrivate::signalOffset(base);
}

/*****************************************************************************
  Properties
 *****************************************************************************/

#ifndef QT_NO_PROPERTIES

/*!
  Sets the value of the object's \a name property to \a value.

  If the property is defined in the class using Q_PROPERTY then
  true is returned on success and false otherwise. If the property
  is not defined using Q_PROPERTY, and therefore not listed in the
  meta-object, it is added as a dynamic property and false is returned.

  Information about all available properties is provided through the
  metaObject() and dynamicPropertyNames().

  Dynamic properties can be queried again using property() and can be
  removed by setting the property value to an invalid QVariant.
  Changing the value of a dynamic property causes a QDynamicPropertyChangeEvent
  to be sent to the object.

  \b{Note:} Dynamic properties starting with "_q_" are reserved for internal
  purposes.

  \sa property(), metaObject(), dynamicPropertyNames(), QMetaProperty::write()
*/
bool QObject::setProperty(const char *name, const QVariant &value)
{
    Q_D(QObject);
    const QMetaObject* meta = metaObject();
    if (!name || !meta)
        return false;

    int id = meta->indexOfProperty(name);
    if (id < 0) {
        if (!d->extraData)
            d->extraData = new QObjectPrivate::ExtraData;

        const int idx = d->extraData->propertyNames.indexOf(name);

        if (!value.isValid()) {
            if (idx == -1)
                return false;
            d->extraData->propertyNames.removeAt(idx);
            d->extraData->propertyValues.removeAt(idx);
        } else {
            if (idx == -1) {
                d->extraData->propertyNames.append(name);
                d->extraData->propertyValues.append(value);
            } else {
                if (value.userType() == d->extraData->propertyValues.at(idx).userType()
                        && value == d->extraData->propertyValues.at(idx))
                    return false;
                d->extraData->propertyValues[idx] = value;
            }
        }

        QDynamicPropertyChangeEvent ev(name);
        QCoreApplication::sendEvent(this, &ev);

        return false;
    }
    QMetaProperty p = meta->property(id);
#ifndef QT_NO_DEBUG
    if (!p.isWritable())
        qWarning("%s::setProperty: Property \"%s\" invalid,"
                 " read-only or does not exist", metaObject()->className(), name);
#endif
    return p.write(this, value);
}

/*!
  Returns the value of the object's \a name property.

  If no such property exists, the returned variant is invalid.

  Information about all available properties is provided through the
  metaObject() and dynamicPropertyNames().

  \sa setProperty(), QVariant::isValid(), metaObject(), dynamicPropertyNames()
*/
QVariant QObject::property(const char *name) const
{
    Q_D(const QObject);
    const QMetaObject* meta = metaObject();
    if (!name || !meta)
        return QVariant();

    int id = meta->indexOfProperty(name);
    if (id < 0) {
        if (!d->extraData)
            return QVariant();
        const int i = d->extraData->propertyNames.indexOf(name);
        return d->extraData->propertyValues.value(i);
    }
    QMetaProperty p = meta->property(id);
#ifndef QT_NO_DEBUG
    if (!p.isReadable())
        qWarning("%s::property: Property \"%s\" invalid or does not exist",
                 metaObject()->className(), name);
#endif
    return p.read(this);
}

/*!
    \since 4.2

    Returns the names of all properties that were dynamically added to
    the object using setProperty().
*/
QList<QByteArray> QObject::dynamicPropertyNames() const
{
    Q_D(const QObject);
    if (d->extraData)
        return d->extraData->propertyNames;
    return QList<QByteArray>();
}

#endif // QT_NO_PROPERTIES


/*****************************************************************************
  QObject debugging output routines.
 *****************************************************************************/

static void dumpRecursive(int level, const QObject *object)
{
    if (object) {
        QByteArray buf;
        buf.fill(' ', level / 2 * 8);
        if (level % 2)
            buf += "    ";
        QString name = object->objectName();
        QString flags = QLatin1String("");
#if 0
        if (qApp->focusWidget() == object)
            flags += 'F';
        if (object->isWidgetType()) {
            QWidget * w = (QWidget *)object;
            if (w->isVisible()) {
                QString t("<%1,%2,%3,%4>");
                flags += t.arg(w->x()).arg(w->y()).arg(w->width()).arg(w->height());
            } else {
                flags += 'I';
            }
        }
#endif
        qDebug("%s%s::%s %s", (const char*)buf, object->metaObject()->className(), name.toLocal8Bit().data(),
               flags.toLatin1().data());
        QObjectList children = object->children();
        if (!children.isEmpty()) {
            for (int i = 0; i < children.size(); ++i)
                dumpRecursive(level+1, children.at(i));
        }
    }
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
/*!
    \overload
    \obsolete

    Dumps a tree of children to the debug output.

    \sa dumpObjectInfo()
*/

void QObject::dumpObjectTree()
{
    const_cast<const QObject *>(this)->dumpObjectTree();
}
#endif

/*!
    Dumps a tree of children to the debug output.

    \note Before Qt 5.9, this function was not const.

    \sa dumpObjectInfo()
*/

void QObject::dumpObjectTree() const
{
    dumpRecursive(0, this);
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
/*!
    \overload
    \obsolete

    Dumps information about signal connections, etc. for this object
    to the debug output.

    \sa dumpObjectTree()
*/

void QObject::dumpObjectInfo()
{
    const_cast<const QObject *>(this)->dumpObjectInfo();
}
#endif

/*!
    Dumps information about signal connections, etc. for this object
    to the debug output.

    \note Before Qt 5.9, this function was not const.

    \sa dumpObjectTree()
*/

void QObject::dumpObjectInfo() const
{
    qDebug("OBJECT %s::%s", metaObject()->className(),
           objectName().isEmpty() ? "unnamed" : objectName().toLocal8Bit().data());

    Q_D(const QObject);
    QBasicMutexLocker locker(signalSlotLock(this));

    // first, look for connections where this object is the sender
    qDebug("  SIGNALS OUT");

    QObjectPrivate::ConnectionData *cd = d->connections.loadRelaxed();
    if (cd && cd->signalVectorCount() > 0) {
        QObjectPrivate::SignalVector *signalVector = cd->signalVector.loadRelaxed();
        for (int signal_index = 0; signal_index < signalVector->count(); ++signal_index) {
            const QObjectPrivate::Connection *c = signalVector->at(signal_index).first.loadRelaxed();
            if (!c)
                continue;
            const QMetaMethod signal = QMetaObjectPrivate::signal(metaObject(), signal_index);
            qDebug("        signal: %s", signal.methodSignature().constData());

            // receivers
            while (c) {
                if (!c->receiver.loadRelaxed()) {
                    qDebug("          <Disconnected receiver>");
                    c = c->nextConnectionList.loadRelaxed();
                    continue;
                }
                if (c->isSlotObject) {
                    qDebug("          <functor or function pointer>");
                    c = c->nextConnectionList.loadRelaxed();
                    continue;
                }
                const QMetaObject *receiverMetaObject = c->receiver.loadRelaxed()->metaObject();
                const QMetaMethod method = receiverMetaObject->method(c->method());
                qDebug("          --> %s::%s %s",
                       receiverMetaObject->className(),
                       c->receiver.loadRelaxed()->objectName().isEmpty() ? "unnamed" : qPrintable(c->receiver.loadRelaxed()->objectName()),
                       method.methodSignature().constData());
                c = c->nextConnectionList.loadRelaxed();
            }
        }
    } else {
        qDebug( "        <None>" );
    }

    // now look for connections where this object is the receiver
    qDebug("  SIGNALS IN");

    if (cd && cd->senders) {
        for (QObjectPrivate::Connection *s = cd->senders; s; s = s->next) {
            QByteArray slotName = QByteArrayLiteral("<unknown>");
            if (!s->isSlotObject) {
                const QMetaMethod slot = metaObject()->method(s->method());
                slotName = slot.methodSignature();
            }
            qDebug("          <-- %s::%s %s",
                   s->sender->metaObject()->className(),
                   s->sender->objectName().isEmpty() ? "unnamed" : qPrintable(s->sender->objectName()),
                   slotName.constData());
        }
    } else {
        qDebug("        <None>");
    }
}

#ifndef QT_NO_USERDATA
static QBasicAtomicInteger<uint> user_data_registration = Q_BASIC_ATOMIC_INITIALIZER(0);

/*!
    \internal
 */
uint QObject::registerUserData()
{
    return user_data_registration.fetchAndAddRelaxed(1);
}

/*!
    \fn QObjectUserData::QObjectUserData()
    \internal
 */

/*!
    \internal
 */
QObjectUserData::~QObjectUserData()
{
}

/*!
    \internal
 */
void QObject::setUserData(uint id, QObjectUserData* data)
{
    Q_D(QObject);
    if (!d->extraData)
        d->extraData = new QObjectPrivate::ExtraData;

    if (d->extraData->userData.size() <= (int) id)
        d->extraData->userData.resize((int) id + 1);
    d->extraData->userData[id] = data;
}

/*!
    \internal
 */
QObjectUserData* QObject::userData(uint id) const
{
    Q_D(const QObject);
    if (!d->extraData)
        return nullptr;
    if ((int)id < d->extraData->userData.size())
        return d->extraData->userData.at(id);
    return nullptr;
}

#endif // QT_NO_USERDATA


#ifndef QT_NO_DEBUG_STREAM
QDebug operator<<(QDebug dbg, const QObject *o)
{
    QDebugStateSaver saver(dbg);
    if (!o)
        return dbg << "QObject(0x0)";
    dbg.nospace() << o->metaObject()->className() << '(' << (const void *)o;
    if (!o->objectName().isEmpty())
        dbg << ", name = " << o->objectName();
    dbg << ')';
    return dbg;
}
#endif

/*!
    \macro Q_CLASSINFO(Name, Value)
    \relates QObject

    This macro associates extra information to the class, which is available
    using QObject::metaObject(). Qt makes only limited use of this feature, in
    the \l{Active Qt}, \l{Qt D-Bus} and \l{Qt QML module}{Qt QML}.

    The extra information takes the form of a \a Name string and a \a Value
    literal string.

    Example:

    \snippet code/src_corelib_kernel_qobject.cpp 35

    \sa QMetaObject::classInfo()
    \sa QAxFactory
    \sa {Using Qt D-Bus Adaptors}
    \sa {Extending QML}
*/

/*!
    \macro Q_INTERFACES(...)
    \relates QObject

    This macro tells Qt which interfaces the class implements. This
    is used when implementing plugins.

    Example:

    \snippet ../widgets/tools/plugandpaint/plugins/basictools/basictoolsplugin.h 1
    \dots
    \snippet ../widgets/tools/plugandpaint/plugins/basictools/basictoolsplugin.h 3

    See the \l{tools/plugandpaint/plugins/basictools}{Plug & Paint
    Basic Tools} example for details.

    \sa Q_DECLARE_INTERFACE(), Q_PLUGIN_METADATA(), {How to Create Qt Plugins}
*/

/*!
    \macro Q_PROPERTY(...)
    \relates QObject

    This macro is used for declaring properties in classes that
    inherit QObject. Properties behave like class data members, but
    they have additional features accessible through the \l
    {Meta-Object System}.

    \snippet code/doc_src_properties.cpp 0

    The property name and type and the \c READ function are required.
    The type can be any type supported by QVariant, or it can be a
    user-defined type.  The other items are optional, but a \c WRITE
    function is common.  The attributes default to true except \c USER,
    which defaults to false.

    For example:

    \snippet code/src_corelib_kernel_qobject.cpp 37

    For more details about how to use this macro, and a more detailed
    example of its use, see the discussion on \l {Qt's Property System}.

    \sa {Qt's Property System}
*/

/*!
    \macro Q_ENUMS(...)
    \relates QObject
    \obsolete

    In new code, you should prefer the use of the Q_ENUM() macro, which makes the
    type available also to the meta type system.
    For instance, QMetaEnum::fromType() will not work with types declared with Q_ENUMS().

    This macro registers one or several enum types to the meta-object
    system.

    If you want to register an enum that is declared in another class,
    the enum must be fully qualified with the name of the class
    defining it. In addition, the class \e defining the enum has to
    inherit QObject as well as declare the enum using Q_ENUMS().

    \sa {Qt's Property System}
*/

/*!
    \macro Q_FLAGS(...)
    \relates QObject
    \obsolete

    This macro registers one or several \l{QFlags}{flags types} with the
    meta-object system. It is typically used in a class definition to declare
    that values of a given enum can be used as flags and combined using the
    bitwise OR operator.

    \note This macro takes care of registering individual flag values
    with the meta-object system, so it is unnecessary to use Q_ENUMS()
    in addition to this macro.

    In new code, you should prefer the use of the Q_FLAG() macro, which makes the
    type available also to the meta type system.

    \sa {Qt's Property System}
*/

/*!
    \macro Q_ENUM(...)
    \relates QObject
    \since 5.5

    This macro registers an enum type with the meta-object system.
    It must be placed after the enum declaration in a class that has the Q_OBJECT or the
    Q_GADGET macro. For namespaces use \l Q_ENUM_NS() instead.

    For example:

    \snippet code/src_corelib_kernel_qobject.cpp 38

    Enumerations that are declared with Q_ENUM have their QMetaEnum registered in the
    enclosing QMetaObject. You can also use QMetaEnum::fromType() to get the QMetaEnum.

    Registered enumerations are automatically registered also to the Qt meta
    type system, making them known to QMetaType without the need to use
    Q_DECLARE_METATYPE(). This will enable useful features; for example, if used
    in a QVariant, you can convert them to strings. Likewise, passing them to
    QDebug will print out their names.

    Mind that the enum values are stored as signed \c int in the meta object system.
    Registering enumerations with values outside the range of values valid for \c int
    will lead to overflows and potentially undefined behavior when accessing them through
    the meta object system. QML, for example, does access registered enumerations through
    the meta object system.

    \sa {Qt's Property System}
*/


/*!
    \macro Q_FLAG(...)
    \relates QObject
    \since 5.5

    This macro registers a single \l{QFlags}{flags type} with the
    meta-object system. It is typically used in a class definition to declare
    that values of a given enum can be used as flags and combined using the
    bitwise OR operator. For namespaces use \l Q_FLAG_NS() instead.

    The macro must be placed after the enum declaration. The declaration of
    the flags type is done using the \l Q_DECLARE_FLAGS() macro.

    For example, in QItemSelectionModel, the
    \l{QItemSelectionModel::SelectionFlags}{SelectionFlags} flag is
    declared in the following way:

    \snippet code/src_corelib_kernel_qobject.cpp 39

    \note The Q_FLAG macro takes care of registering individual flag values
    with the meta-object system, so it is unnecessary to use Q_ENUM()
    in addition to this macro.

    \sa {Qt's Property System}
*/

/*!
    \macro Q_ENUM_NS(...)
    \relates QObject
    \since 5.8

    This macro registers an enum type with the meta-object system.
    It must be placed after the enum declaration in a namespace that
    has the Q_NAMESPACE macro. It is the same as \l Q_ENUM but in a
    namespace.

    Enumerations that are declared with Q_ENUM_NS have their QMetaEnum
    registered in the enclosing QMetaObject. You can also use
    QMetaEnum::fromType() to get the QMetaEnum.

    Registered enumerations are automatically registered also to the Qt meta
    type system, making them known to QMetaType without the need to use
    Q_DECLARE_METATYPE(). This will enable useful features; for example, if
    used in a QVariant, you can convert them to strings. Likewise, passing them
    to QDebug will print out their names.

    Mind that the enum values are stored as signed \c int in the meta object system.
    Registering enumerations with values outside the range of values valid for \c int
    will lead to overflows and potentially undefined behavior when accessing them through
    the meta object system. QML, for example, does access registered enumerations through
    the meta object system.

    \sa {Qt's Property System}
*/


/*!
    \macro Q_FLAG_NS(...)
    \relates QObject
    \since 5.8

    This macro registers a single \l{QFlags}{flags type} with the
    meta-object system. It is used in a namespace that has the
    Q_NAMESPACE macro, to declare that values of a given enum can be
    used as flags and combined using the bitwise OR operator.
    It is the same as \l Q_FLAG but in a namespace.

    The macro must be placed after the enum declaration.

    \note The Q_FLAG_NS macro takes care of registering individual flag
    values with the meta-object system, so it is unnecessary to use
    Q_ENUM_NS() in addition to this macro.

    \sa {Qt's Property System}
*/


/*!
    \macro Q_OBJECT
    \relates QObject

    The Q_OBJECT macro must appear in the private section of a class
    definition that declares its own signals and slots or that uses
    other services provided by Qt's meta-object system.

    For example:

    \snippet signalsandslots/signalsandslots.h 1
    \codeline
    \snippet signalsandslots/signalsandslots.h 2
    \snippet signalsandslots/signalsandslots.h 3

    \note This macro requires the class to be a subclass of QObject. Use
    Q_GADGET instead of Q_OBJECT to enable the meta object system's support
    for enums in a class that is not a QObject subclass.

    \sa {Meta-Object System}, {Signals and Slots}, {Qt's Property System}
*/

/*!
    \macro Q_GADGET
    \relates QObject

    The Q_GADGET macro is a lighter version of the Q_OBJECT macro for classes
    that do not inherit from QObject but still want to use some of the
    reflection capabilities offered by QMetaObject. Just like the Q_OBJECT
    macro, it must appear in the private section of a class definition.

    Q_GADGETs can have Q_ENUM, Q_PROPERTY and Q_INVOKABLE, but they cannot have
    signals or slots.

    Q_GADGET makes a class member, \c{staticMetaObject}, available.
    \c{staticMetaObject} is of type QMetaObject and provides access to the
    enums declared with Q_ENUMS.
*/

/*!
    \macro Q_NAMESPACE
    \relates QObject
    \since 5.8

    The Q_NAMESPACE macro can be used to add QMetaObject capabilities
    to a namespace.

    Q_NAMESPACEs can have Q_CLASSINFO, Q_ENUM_NS, Q_FLAG_NS, but they
    cannot have Q_ENUM, Q_FLAG, Q_PROPERTY, Q_INVOKABLE, signals nor slots.

    Q_NAMESPACE makes an external variable, \c{staticMetaObject}, available.
    \c{staticMetaObject} is of type QMetaObject and provides access to the
    enums declared with Q_ENUM_NS/Q_FLAG_NS.

    For example:

    \code
    namespace test {
    Q_NAMESPACE
    ...
    \endcode

    \sa Q_NAMESPACE_EXPORT
*/

/*!
    \macro Q_NAMESPACE_EXPORT(EXPORT_MACRO)
    \relates QObject
    \since 5.14

    The Q_NAMESPACE_EXPORT macro can be used to add QMetaObject capabilities
    to a namespace.

    It works exactly like the Q_NAMESPACE macro. However, the external
    \c{staticMetaObject} variable that gets defined in the namespace
    is declared with the supplied \a EXPORT_MACRO qualifier. This is
    useful if the object needs to be exported from a dynamic library.

    For example:

    \code
    namespace test {
    Q_NAMESPACE_EXPORT(EXPORT_MACRO)
    ...
    \endcode

    \sa Q_NAMESPACE, {Creating Shared Libraries}
*/

/*!
    \macro Q_SIGNALS
    \relates QObject

    Use this macro to replace the \c signals keyword in class
    declarations, when you want to use Qt Signals and Slots with a
    \l{3rd Party Signals and Slots} {3rd party signal/slot mechanism}.

    The macro is normally used when \c no_keywords is specified with
    the \c CONFIG variable in the \c .pro file, but it can be used
    even when \c no_keywords is \e not specified.
*/

/*!
    \macro Q_SIGNAL
    \relates QObject

    This is an additional macro that allows you to mark a single
    function as a signal. It can be quite useful, especially when you
    use a 3rd-party source code parser which doesn't understand a \c
    signals or \c Q_SIGNALS groups.

    Use this macro to replace the \c signals keyword in class
    declarations, when you want to use Qt Signals and Slots with a
    \l{3rd Party Signals and Slots} {3rd party signal/slot mechanism}.

    The macro is normally used when \c no_keywords is specified with
    the \c CONFIG variable in the \c .pro file, but it can be used
    even when \c no_keywords is \e not specified.
*/

/*!
    \macro Q_SLOTS
    \relates QObject

    Use this macro to replace the \c slots keyword in class
    declarations, when you want to use Qt Signals and Slots with a
    \l{3rd Party Signals and Slots} {3rd party signal/slot mechanism}.

    The macro is normally used when \c no_keywords is specified with
    the \c CONFIG variable in the \c .pro file, but it can be used
    even when \c no_keywords is \e not specified.
*/

/*!
    \macro Q_SLOT
    \relates QObject

    This is an additional macro that allows you to mark a single
    function as a slot. It can be quite useful, especially when you
    use a 3rd-party source code parser which doesn't understand a \c
    slots or \c Q_SLOTS groups.

    Use this macro to replace the \c slots keyword in class
    declarations, when you want to use Qt Signals and Slots with a
    \l{3rd Party Signals and Slots} {3rd party signal/slot mechanism}.

    The macro is normally used when \c no_keywords is specified with
    the \c CONFIG variable in the \c .pro file, but it can be used
    even when \c no_keywords is \e not specified.
*/

/*!
    \macro Q_EMIT
    \relates QObject

    Use this macro to replace the \c emit keyword for emitting
    signals, when you want to use Qt Signals and Slots with a
    \l{3rd Party Signals and Slots} {3rd party signal/slot mechanism}.

    The macro is normally used when \c no_keywords is specified with
    the \c CONFIG variable in the \c .pro file, but it can be used
    even when \c no_keywords is \e not specified.
*/

/*!
    \macro Q_INVOKABLE
    \relates QObject

    Apply this macro to declarations of member functions to allow them to
    be invoked via the meta-object system. The macro is written before
    the return type, as shown in the following example:

    \snippet qmetaobject-invokable/window.h Window class with invokable method

    The \c invokableMethod() function is marked up using Q_INVOKABLE, causing
    it to be registered with the meta-object system and enabling it to be
    invoked using QMetaObject::invokeMethod().
    Since \c normalMethod() function is not registered in this way, it cannot
    be invoked using QMetaObject::invokeMethod().

    If an invokable member function returns a pointer to a QObject or a
    subclass of QObject and it is invoked from QML, special ownership rules
    apply. See \l{qtqml-cppintegration-data.html}{Data Type Conversion Between QML and C++}
    for more information.
*/

/*!
    \macro Q_REVISION
    \relates QObject

    Apply this macro to declarations of member functions to tag them with a
    revision number in the meta-object system. The macro is written before
    the return type, as shown in the following example:

    \snippet qmetaobject-revision/window.h Window class with revision

    This is useful when using the meta-object system to dynamically expose
    objects to another API, as you can match the version expected by multiple
    versions of the other API. Consider the following simplified example:

    \snippet qmetaobject-revision/main.cpp Window class using revision

    Using the same Window class as the previous example, the newProperty and
    newMethod would only be exposed in this code when the expected version is
    1 or greater.

    Since all methods are considered to be in revision 0 if untagged, a tag
    of Q_REVISION(0) is invalid and ignored.

    This tag is not used by the meta-object system itself. Currently this is only
    used by the QtQml module.

    For a more generic string tag, see \l QMetaMethod::tag()

    \sa QMetaMethod::revision()
*/

/*!
    \macro Q_SET_OBJECT_NAME(Object)
    \relates QObject
    \since 5.0

    This macro assigns \a Object the objectName "Object".

    It doesn't matter whether \a Object is a pointer or not, the
    macro figures that out by itself.

    \sa QObject::objectName()
*/

/*!
    \macro QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
    \relates QObject
    \since 5.8

    Defining this macro will disable narrowing and floating-point-to-integral
    conversions between the arguments carried by a signal and the arguments
    accepted by a slot, when the signal and the slot are connected using the
    PMF-based syntax.

    \sa QObject::connect
*/

/*!
    \typedef QObjectList
    \relates QObject

    Synonym for QList<QObject *>.
*/

void qDeleteInEventHandler(QObject *o)
{
    delete o;
}

/*!
    \fn template<typename PointerToMemberFunction> QMetaObject::Connection QObject::connect(const QObject *sender, PointerToMemberFunction signal, const QObject *receiver, PointerToMemberFunction method, Qt::ConnectionType type)
    \overload connect()
    \threadsafe

    Creates a connection of the given \a type from the \a signal in
    the \a sender object to the \a method in the \a receiver object.
    Returns a handle to the connection that can be used to disconnect
    it later.

    The signal must be a function declared as a signal in the header.
    The slot function can be any member function that can be connected
    to the signal.
    A slot can be connected to a given signal if the signal has at
    least as many arguments as the slot, and there is an implicit
    conversion between the types of the corresponding arguments in the
    signal and the slot.

    Example:

    \snippet code/src_corelib_kernel_qobject.cpp 44

    This example ensures that the label always displays the current
    line edit text.

    A signal can be connected to many slots and signals. Many signals
    can be connected to one slot.

    If a signal is connected to several slots, the slots are activated
    in the same order as the order the connection was made, when the
    signal is emitted

    The function returns an handle to a connection if it successfully
    connects the signal to the slot. The Connection handle will be invalid
    if it cannot create the connection, for example, if QObject is unable
    to verify the existence of \a signal (if it was not declared as a signal)
    You can check if the QMetaObject::Connection is valid by casting it to a bool.

    By default, a signal is emitted for every connection you make;
    two signals are emitted for duplicate connections. You can break
    all of these connections with a single disconnect() call.
    If you pass the Qt::UniqueConnection \a type, the connection will only
    be made if it is not a duplicate. If there is already a duplicate
    (exact same signal to the exact same slot on the same objects),
    the connection will fail and connect will return an invalid QMetaObject::Connection.

    The optional \a type parameter describes the type of connection
    to establish. In particular, it determines whether a particular
    signal is delivered to a slot immediately or queued for delivery
    at a later time. If the signal is queued, the parameters must be
    of types that are known to Qt's meta-object system, because Qt
    needs to copy the arguments to store them in an event behind the
    scenes. If you try to use a queued connection and get the error
    message

    \snippet code/src_corelib_kernel_qobject.cpp 25

    make sure to declare the argument type with Q_DECLARE_METATYPE

    Overloaded functions can be resolved with help of \l qOverload.

    \sa {Differences between String-Based and Functor-Based Connections}
 */

/*!
    \fn template<typename PointerToMemberFunction, typename Functor> QMetaObject::Connection QObject::connect(const QObject *sender, PointerToMemberFunction signal, Functor functor)

    \threadsafe
    \overload connect()

    Creates a connection from \a signal in
    \a sender object to \a functor, and returns a handle to the connection

    The signal must be a function declared as a signal in the header.
    The slot function can be any function or functor that can be connected
    to the signal.
    A function can be connected to a given signal if the signal has at
    least as many argument as the slot. A functor can be connected to a signal
    if they have exactly the same number of arguments. There must exist implicit
    conversion between the types of the corresponding arguments in the
    signal and the slot.

    Example:

    \snippet code/src_corelib_kernel_qobject.cpp 45

    Lambda expressions can also be used:

    \snippet code/src_corelib_kernel_qobject.cpp 46

    The connection will automatically disconnect if the sender is destroyed.
    However, you should take care that any objects used within the functor
    are still alive when the signal is emitted.

    Overloaded functions can be resolved with help of \l qOverload.

 */

/*!
    \fn template<typename PointerToMemberFunction, typename Functor> QMetaObject::Connection QObject::connect(const QObject *sender, PointerToMemberFunction signal, const QObject *context, Functor functor, Qt::ConnectionType type)

    \threadsafe
    \overload connect()

    \since 5.2

    Creates a connection of a given \a type from \a signal in
    \a sender object to \a functor to be placed in a specific event
    loop of \a context, and returns a handle to the connection.

    \note Qt::UniqueConnections do not work for lambdas, non-member functions
    and functors; they only apply to connecting to member functions.

    The signal must be a function declared as a signal in the header.
    The slot function can be any function or functor that can be connected
    to the signal.
    A function can be connected to a given signal if the signal has at
    least as many argument as the slot. A functor can be connected to a signal
    if they have exactly the same number of arguments. There must exist implicit
    conversion between the types of the corresponding arguments in the
    signal and the slot.

    Example:

    \snippet code/src_corelib_kernel_qobject.cpp 50

    Lambda expressions can also be used:

    \snippet code/src_corelib_kernel_qobject.cpp 51

    The connection will automatically disconnect if the sender or the context
    is destroyed.
    However, you should take care that any objects used within the functor
    are still alive when the signal is emitted.

    Overloaded functions can be resolved with help of \l qOverload.
 */

/*!
    \internal

    Implementation of the template version of connect

    \a sender is the sender object
    \a signal is a pointer to a pointer to a member signal of the sender
    \a receiver is the receiver object, may not be \nullptr, will be equal to sender when
                connecting to a static function or a functor
    \a slot a pointer only used when using Qt::UniqueConnection
    \a type the Qt::ConnectionType passed as argument to connect
    \a types an array of integer with the metatype id of the parameter of the signal
             to be used with queued connection
             must stay valid at least for the whole time of the connection, this function
             do not take ownership. typically static data.
             If \nullptr, then the types will be computed when the signal is emit in a queued
             connection from the types from the signature.
    \a senderMetaObject is the metaobject used to lookup the signal, the signal must be in
                        this metaobject
 */
QMetaObject::Connection QObject::connectImpl(const QObject *sender, void **signal,
                                             const QObject *receiver, void **slot,
                                             QtPrivate::QSlotObjectBase *slotObj, Qt::ConnectionType type,
                                             const int *types, const QMetaObject *senderMetaObject)
{
    if (!signal) {
        qWarning("QObject::connect: invalid nullptr parameter");
        if (slotObj)
            slotObj->destroyIfLastRef();
        return QMetaObject::Connection();
    }

    int signal_index = -1;
    void *args[] = { &signal_index, signal };
    for (; senderMetaObject && signal_index < 0; senderMetaObject = senderMetaObject->superClass()) {
        senderMetaObject->static_metacall(QMetaObject::IndexOfMethod, 0, args);
        if (signal_index >= 0 && signal_index < QMetaObjectPrivate::get(senderMetaObject)->signalCount)
            break;
    }
    if (!senderMetaObject) {
        qWarning("QObject::connect: signal not found in %s", sender->metaObject()->className());
        slotObj->destroyIfLastRef();
        return QMetaObject::Connection(nullptr);
    }
    signal_index += QMetaObjectPrivate::signalOffset(senderMetaObject);
    return QObjectPrivate::connectImpl(sender, signal_index, receiver, slot, slotObj, type, types, senderMetaObject);
}

/*!
    \internal

    Internal version of connect used by the template version of QObject::connect (called via connectImpl) and
    also used by the QObjectPrivate::connect version used by QML. The signal_index is expected to be relative
    to the number of signals.
 */
QMetaObject::Connection QObjectPrivate::connectImpl(const QObject *sender, int signal_index,
                                             const QObject *receiver, void **slot,
                                             QtPrivate::QSlotObjectBase *slotObj, Qt::ConnectionType type,
                                             const int *types, const QMetaObject *senderMetaObject)
{
    if (!sender || !receiver || !slotObj || !senderMetaObject) {
        const char *senderString = sender ? sender->metaObject()->className()
                                          : senderMetaObject ? senderMetaObject->className()
                                          : "Unknown";
        const char *receiverString = receiver ? receiver->metaObject()->className()
                                              : "Unknown";
        qWarning("QObject::connect(%s, %s): invalid nullptr parameter", senderString, receiverString);
        if (slotObj)
            slotObj->destroyIfLastRef();
        return QMetaObject::Connection();
    }

    QObject *s = const_cast<QObject *>(sender);
    QObject *r = const_cast<QObject *>(receiver);

    QOrderedMutexLocker locker(signalSlotLock(sender),
                               signalSlotLock(receiver));

    if (type & Qt::UniqueConnection && slot && QObjectPrivate::get(s)->connections.loadRelaxed()) {
        QObjectPrivate::ConnectionData *connections = QObjectPrivate::get(s)->connections.loadRelaxed();
        if (connections->signalVectorCount() > signal_index) {
            const QObjectPrivate::Connection *c2 = connections->signalVector.loadRelaxed()->at(signal_index).first.loadRelaxed();

            while (c2) {
                if (c2->receiver.loadRelaxed() == receiver && c2->isSlotObject && c2->slotObj->compare(slot)) {
                    slotObj->destroyIfLastRef();
                    return QMetaObject::Connection();
                }
                c2 = c2->nextConnectionList.loadRelaxed();
            }
        }
        type = static_cast<Qt::ConnectionType>(type ^ Qt::UniqueConnection);
    }

    std::unique_ptr<QObjectPrivate::Connection> c{new QObjectPrivate::Connection};
    c->sender = s;
    c->signal_index = signal_index;
    QThreadData *td = r->d_func()->threadData;
    td->ref();
    c->receiverThreadData.storeRelaxed(td);
    c->receiver.storeRelaxed(r);
    c->slotObj = slotObj;
    c->connectionType = type;
    c->isSlotObject = true;
    if (types) {
        c->argumentTypes.storeRelaxed(types);
        c->ownArgumentTypes = false;
    }

    QObjectPrivate::get(s)->addConnection(signal_index, c.get());
    QMetaObject::Connection ret(c.release());
    locker.unlock();

    QMetaMethod method = QMetaObjectPrivate::signal(senderMetaObject, signal_index);
    Q_ASSERT(method.isValid());
    s->connectNotify(method);

    return ret;
}

/*!
    Disconnect a connection.

    If the \a connection is invalid or has already been disconnected, do nothing
    and return false.

   \sa connect()
 */
bool QObject::disconnect(const QMetaObject::Connection &connection)
{
    QObjectPrivate::Connection *c = static_cast<QObjectPrivate::Connection *>(connection.d_ptr);

    if (!c)
        return false;
    QObject *receiver = c->receiver.loadRelaxed();
    if (!receiver)
        return false;

    QBasicMutex *senderMutex = signalSlotLock(c->sender);
    QBasicMutex *receiverMutex = signalSlotLock(receiver);

    QObjectPrivate::ConnectionData *connections;
    {
        QOrderedMutexLocker locker(senderMutex, receiverMutex);

        // load receiver once again and recheck to ensure nobody else has removed the connection in the meantime
        receiver = c->receiver.loadRelaxed();
        if (!receiver)
            return false;

        connections = QObjectPrivate::get(c->sender)->connections.loadRelaxed();
        Q_ASSERT(connections);
        connections->removeConnection(c);

        c->sender->disconnectNotify(QMetaObjectPrivate::signal(c->sender->metaObject(), c->signal_index));
        // We must not hold the receiver mutex, else we risk dead-locking; we also only need the sender mutex
        // It is however vital to hold the senderMutex before calling cleanOrphanedConnections, as otherwise
        // another thread might modify/delete the connection
        if (receiverMutex != senderMutex) {
            receiverMutex->unlock();
        }
        connections->cleanOrphanedConnections(c->sender, QObjectPrivate::ConnectionData::AlreadyLockedAndTemporarilyReleasingLock);
        senderMutex->unlock();  // now both sender and receiver mutex have been manually unlocked
        locker.dismiss(); // so we dismiss the QOrderedMutexLocker
    }

    const_cast<QMetaObject::Connection &>(connection).d_ptr = nullptr;
    c->deref(); // has been removed from the QMetaObject::Connection object

    return true;
}

/*! \fn template<typename PointerToMemberFunction> bool QObject::disconnect(const QObject *sender, PointerToMemberFunction signal, const QObject *receiver, PointerToMemberFunction method)
    \overload disconnect()
    \threadsafe

    Disconnects \a signal in object \a sender from \a method in object
    \a receiver. Returns \c true if the connection is successfully broken;
    otherwise returns \c false.

    A signal-slot connection is removed when either of the objects
    involved are destroyed.

    disconnect() is typically used in three ways, as the following
    examples demonstrate.
    \list 1
    \li Disconnect everything connected to an object's signals:

       \snippet code/src_corelib_kernel_qobject.cpp 26

    \li Disconnect everything connected to a specific signal:

       \snippet code/src_corelib_kernel_qobject.cpp 47

    \li Disconnect a specific receiver:

       \snippet code/src_corelib_kernel_qobject.cpp 30

    \li Disconnect a connection from one specific signal to a specific slot:

       \snippet code/src_corelib_kernel_qobject.cpp 48


    \endlist

    \nullptr may be used as a wildcard, meaning "any signal", "any receiving
    object", or "any slot in the receiving object", respectively.

    The \a sender may never be \nullptr. (You cannot disconnect signals
    from more than one object in a single call.)

    If \a signal is \nullptr, it disconnects \a receiver and \a method from
    any signal. If not, only the specified signal is disconnected.

    If \a receiver is \nullptr, it disconnects anything connected to \a
    signal. If not, slots in objects other than \a receiver are not
    disconnected.

    If \a method is \nullptr, it disconnects anything that is connected to \a
    receiver. If not, only slots named \a method will be disconnected,
    and all other slots are left alone. The \a method must be \nullptr
    if \a receiver is left out, so you cannot disconnect a
    specifically-named slot on all objects.

    \note It is not possible to use this overload to disconnect signals
    connected to functors or lambda expressions. That is because it is not
    possible to compare them. Instead, use the overload that takes a
    QMetaObject::Connection

    \sa connect()
*/

bool QObject::disconnectImpl(const QObject *sender, void **signal, const QObject *receiver, void **slot, const QMetaObject *senderMetaObject)
{
    if (sender == nullptr || (receiver == nullptr && slot != nullptr)) {
        qWarning("QObject::disconnect: Unexpected nullptr parameter");
        return false;
    }

    int signal_index = -1;
    if (signal) {
        void *args[] = { &signal_index, signal };
        for (; senderMetaObject && signal_index < 0; senderMetaObject = senderMetaObject->superClass()) {
            senderMetaObject->static_metacall(QMetaObject::IndexOfMethod, 0, args);
            if (signal_index >= 0 && signal_index < QMetaObjectPrivate::get(senderMetaObject)->signalCount)
                break;
        }
        if (!senderMetaObject) {
            qWarning("QObject::disconnect: signal not found in %s", sender->metaObject()->className());
            return false;
        }
        signal_index += QMetaObjectPrivate::signalOffset(senderMetaObject);
    }

    return QMetaObjectPrivate::disconnect(sender, signal_index, senderMetaObject, receiver, -1, slot);
}

/*!
 \internal
 Used by QML to connect a signal by index to a slot implemented in JavaScript
 (wrapped in a custom QSlotObjectBase subclass).

 This version of connect assumes that sender and receiver are the same object.

 The signal_index is an index relative to the number of methods.
 */
QMetaObject::Connection QObjectPrivate::connect(const QObject *sender, int signal_index, QtPrivate::QSlotObjectBase *slotObj, Qt::ConnectionType type)
{
    return QObjectPrivate::connect(sender, signal_index, sender, slotObj, type);
}

/*!
 \internal
 Used by QML to connect a signal by index to a slot implemented in JavaScript
 (wrapped in a custom QSlotObjectBase subclass).

 This is an overload that should be used when \a sender and \a receiver are
 different objects.

 The signal_index is an index relative to the number of methods.
 */
QMetaObject::Connection QObjectPrivate::connect(const QObject *sender, int signal_index,
                                                const QObject *receiver,
                                                QtPrivate::QSlotObjectBase *slotObj,
                                                Qt::ConnectionType type)
{
    if (!sender) {
        qWarning("QObject::connect: invalid nullptr parameter");
        if (slotObj)
            slotObj->destroyIfLastRef();
        return QMetaObject::Connection();
    }
    const QMetaObject *senderMetaObject = sender->metaObject();
    signal_index = methodIndexToSignalIndex(&senderMetaObject, signal_index);

    return QObjectPrivate::connectImpl(sender, signal_index, receiver, /*slot*/ nullptr, slotObj,
                                       type, /*types*/ nullptr, senderMetaObject);
}

/*!
 \internal
 Used by QML to disconnect a signal by index that's connected to a slot implemented in JavaScript (wrapped in a custom QSlotObjectBase subclass)
 In the QML case the slot is not a pointer to a pointer to the function to disconnect, but instead it is a pointer to an array of internal values
 required for the disconnect.

 This version of disconnect assumes that sender and receiver are the same object.
 */
bool QObjectPrivate::disconnect(const QObject *sender, int signal_index, void **slot)
{
    return QObjectPrivate::disconnect(sender, signal_index, sender, slot);
}

/*!
 \internal

 Used by QML to disconnect a signal by index that's connected to a slot
 implemented in JavaScript (wrapped in a custom QSlotObjectBase subclass) In the
 QML case the slot is not a pointer to a pointer to the function to disconnect,
 but instead it is a pointer to an array of internal values required for the
 disconnect.

 This is an overload that should be used when \a sender and \a receiver are
 different objects.
 */
bool QObjectPrivate::disconnect(const QObject *sender, int signal_index, const QObject *receiver,
                                void **slot)
{
    const QMetaObject *senderMetaObject = sender->metaObject();
    signal_index = methodIndexToSignalIndex(&senderMetaObject, signal_index);

    return QMetaObjectPrivate::disconnect(sender, signal_index, senderMetaObject, receiver, -1,
                                          slot);
}

/*! \class QMetaObject::Connection
    \inmodule QtCore
     Represents a handle to a signal-slot (or signal-functor) connection.

     It can be used to check if the connection is valid and to disconnect it using
     QObject::disconnect(). For a signal-functor connection without a context object,
     it is the only way to selectively disconnect that connection.

     As Connection is just a handle, the underlying signal-slot connection is unaffected
     when Connection is destroyed or reassigned.
 */

/*!
    Create a copy of the handle to the \a other connection
 */
QMetaObject::Connection::Connection(const QMetaObject::Connection &other) : d_ptr(other.d_ptr)
{
    if (d_ptr)
        static_cast<QObjectPrivate::Connection *>(d_ptr)->ref();
}

/*!
    Assigns \a other to this connection and returns a reference to this connection.
*/
QMetaObject::Connection& QMetaObject::Connection::operator=(const QMetaObject::Connection& other)
{
    if (other.d_ptr != d_ptr) {
        if (d_ptr)
            static_cast<QObjectPrivate::Connection *>(d_ptr)->deref();
        d_ptr = other.d_ptr;
        if (other.d_ptr)
            static_cast<QObjectPrivate::Connection *>(other.d_ptr)->ref();
    }
    return *this;
}

/*!
    Creates a Connection instance.
*/

QMetaObject::Connection::Connection() : d_ptr(nullptr) {}

/*!
    Destructor for QMetaObject::Connection.
*/
QMetaObject::Connection::~Connection()
{
    if (d_ptr)
        static_cast<QObjectPrivate::Connection *>(d_ptr)->deref();
}

/*! \internal Returns true if the object is still connected */
bool QMetaObject::Connection::isConnected_helper() const
{
    Q_ASSERT(d_ptr);    // we're only called from operator RestrictedBool() const
    QObjectPrivate::Connection *c = static_cast<QObjectPrivate::Connection *>(d_ptr);

    return c->receiver.loadRelaxed();
}


/*!
    \fn QMetaObject::Connection::operator bool() const

    Returns \c true if the connection is valid.

    The connection is valid if the call to QObject::connect succeeded.
    The connection is invalid if QObject::connect was not able to find
    the signal or the slot, or if the arguments do not match.
 */

QT_END_NAMESPACE

#include "moc_qnamespace.cpp"
#include "moc_qobject.cpp"
