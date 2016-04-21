/****************************************************************************
**
** Copyright (C) 2016 Pelagicore AG
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Pelagicore Application Manager.
**
** $QT_BEGIN_LICENSE:GPL-QTAS$
** Commercial License Usage
** Licensees holding valid commercial Qt Automotive Suite licenses may use
** this file in accordance with the commercial license agreement provided
** with the Software or, alternatively, in accordance with the terms
** contained in a written agreement between you and The Qt Company.  For
** licensing terms and conditions see https://www.qt.io/terms-conditions.
** For further information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
** SPDX-License-Identifier: GPL-3.0
**
****************************************************************************/

#include <QGuiApplication>
#include <QRegularExpression>
#include <QQuickView>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQmlEngine>
#include <QVariant>

#ifndef AM_SINGLE_PROCESS_MODE
#  if QT_VERSION >= QT_VERSION_CHECK(5,7,0)
#    include "waylandcompositor.h"
#  else
#    include "waylandcompositor-old.h"
#  endif
#endif

#include "global.h"
#include "application.h"
#include "applicationmanager.h"
#include "abstractruntime.h"
#include "window.h"
#include "windowmanager.h"
#include "windowmanager_p.h"
#include "waylandwindow.h"
#include "inprocesswindow.h"
#include "dbus-policy.h"
#include "qml-utilities.h"
#include "systemmonitor.h"


#define AM_AUTHENTICATE_DBUS(RETURN_TYPE) \
    do { \
        if (!checkDBusPolicy(this, d->dbusPolicy, __FUNCTION__)) \
            return RETURN_TYPE(); \
    } while (false);

/*!
    \class WindowManager
    \internal
*/

/*!
    \qmltype WindowManager
    \inqmlmodule io.qt.ApplicationManager 1.0
    \brief The WindowManager singleton

    This singleton class is the window managing part of the application manager. It provides a QML
    API only.

    To make QML programmers lifes easier, the class is derived from \c QAbstractListModel,
    so you can directly use this singleton as a model in your window views.

    Each item in this model corresponds to an actual window surface. Please not that a single
    application can have multiple surfaces: the \c applicationId role is not unique within this model!

    \keyword WindowManager Roles

    The following roles are available in this model:

    \table
    \header
        \li Role name
        \li Type
        \li Description
    \row
        \li \c applicationId
        \li \c string
        \li The unique id of an application represented as a string in reverse-dns form (e.g.
            \c com.pelagicore.foo). This can be used to look up information about the application
            in the ApplicationManager model.
    \row
        \li \c windowItem
        \li \c Item
        \li The QtQuick Item representing the window surface of the application - used to actually
            composite the window on the screen.
    \row
        \li \c isFullscreen
        \li \c bool
        \li A boolean value telling if the app's surface is being displayed in fullscreen mode.
    \row
        \li \c isMapped
        \li \c bool
        \li A boolean value telling if the app's surface is mapped (visible).
    \endtable

    The QML import for this singleton is

    \c{import io.qt.ApplicationManager 1.0}

    After importing, you can just use the WindowManager singleton as shown in the example below.
    It shows how to implement a basic fullscreen window compositor that already supports window
    show and hide animations:

    \qml
    import QtQuick 2.0
    import io.qt.ApplicationManager 1.0

    // simple solution for a full-screen setup
    Item {
        id: fullscreenView

        MouseArea {
            // without this area, clicks would go "through" the surfaces
            id: filterMouseEventsForWindowContainer
            anchors.fill: parent
            enabled: false
        }

        Item {
            id: windowContainer
            anchors.fill: parent
            state: "closed"

            function windowReadyHandler(index, window) {
                filterMouseEventsForWindowContainer.enabled = true
                windowContainer.state = ""
                windowContainer.windowItem = window
            }
            function windowClosingHandler(index, window) {
                if (window === windowContainer.windowItem) {
                    // start close animation
                    windowContainer.state = "closed"
                } else {
                    // immediately close anything which is not handled by this container
                    WindowManager.releasewindow(index, window)
                }
            }
            function windowLostHandler(index, window) {
                if (windowContainer.windowItem === window) {
                    windowContainer.windowItem = placeHolder
                }
            }
            Component.onCompleted: {
                WindowManager.windowReady.connect(windowReadyHandler)
                WindowManager.windowClosing.connect(windowClosingHandler)
                WindowManager.windowLost.connect(windowLostHandler)
            }

            property Item windowItem: placeHolder
            onWindowItemChanged: {
                windowItem.parent = windowContainer  // reset parent in any case
            }

            Item {
                id: placeHolder;
            }

            // a different syntax for 'anchors.fill: parent' due to the volatile nature of windowItem
            Binding { target: windowContainer.windowItem; property: "x"; value: windowContainer.x }
            Binding { target: windowContainer.windowItem; property: "y"; value: windowContainer.y }
            Binding { target: windowContainer.windowItem; property: "width"; value: windowContainer.width }
            Binding { target: windowContainer.windowItem; property: "height"; value: windowContainer.height }

            transitions: [
                Transition {
                    to: "closed"
                    SequentialAnimation {
                        alwaysRunToEnd: true

                        // your closing animation goes here
                        // ...

                        ScriptAction {
                            script: {
                                windowContainer.windowItem.visible = false;
                                WindowManager.releaseWindow(windowContainer.windowItem);
                                filterMouseEventsForWindowContainer.enabled = false;
                            }
                        }
                    }
                },
                Transition {
                    from: "closed"
                    SequentialAnimation {
                        alwaysRunToEnd: true

                        // your opening animation goes here
                        // ...
                    }
                }
            ]
        }
    }
    \endqml

*/

/*!
    \qmlsignal WindowManager::windowReady(int index, Item window)

    This signal is emitted after a new \a window surface has been created. Most likely due
    to an application launch.

    More information about this window (e.g. the corresponding application) can be retrieved via the
    model \a index.
*/

/*!
    \qmlsignal WindowManager::windowClosing(int index, Item window)

    This signal is emitted when a \a window surface is unmapped.

    Either the client application closed the window, or it exited and all its Wayland surfaces got
    implicitly unmapped.
    When using the \c qml-inprocess runtime this signal will also be emitted when the \c close
    signal of the fake surface is triggered.

    The actual surface can still be used for animations, since it will not be deleted right
    after this signal is emitted.

    The system-ui is required to answer this signal by calling releaseWindow as soon as the QML
    animations on this window surface are finished. Neglecting to do so will result in resource leaks!

    More information about this window (e.g. the corresponding application) can be retrieved via the
    model \a index.

    \sa windowLost
*/

/*!
    \qmlsignal WindowManager::windowLost(int index, Item window)

    After this signal has been fired, the \a window surface is not usable anymore. This should only
    happen after releaseWindow has been called.

    More information about this window (e.g. the corresponding application) can be retrieved via the
    model \a index.
*/

/*!
    \qmlsignal WindowManager::raiseApplicationWindow(string applicationId)

    This signal is emitted when an application start is triggered for the already running application
    identified by \a applicationId via the ApplicationManager.
*/


namespace {
enum Roles
{
    Id = Qt::UserRole + 1000,
    WindowItem,
    IsFullscreen,
    IsMapped
};
}


WindowManager *WindowManager::s_instance = 0;


WindowManager *WindowManager::createInstance(QQmlEngine *qmlEngine, bool forceSingleProcess, const QString &waylandSocketName)
{
    if (s_instance)
        qFatal("WindowManager::createInstance() was called a second time.");
    return s_instance = new WindowManager(qmlEngine, forceSingleProcess, waylandSocketName);
}

WindowManager *WindowManager::instance()
{
    if (!s_instance)
        qFatal("WindowManager::instance() was called before createInstance().");
    return s_instance;
}

QObject *WindowManager::instanceForQml(QQmlEngine *qmlEngine, QJSEngine *)
{
    if (qmlEngine)
        retakeSingletonOwnershipFromQmlEngine(qmlEngine, instance());
    return instance();
}

/*!
    \qmlproperty bool WindowManager::runningOnDesktop

    \c true if running on a classic desktop window manager (Windows, Mac OS X or X11) and
    \c false otherwise.
*/
bool WindowManager::isRunningOnDesktop() const
{
#if defined(Q_OS_WIN) || defined(Q_OS_OSX)
    return true;
#else
    return qApp->platformName() == qSL("xcb");
#endif
}

WindowManager::WindowManager(QQmlEngine *qmlEngine, bool forceSingleProcess, const QString &waylandSocketName)
    : QAbstractListModel(qmlEngine)
    , d(new WindowManagerPrivate())
{
#if !defined(AM_SINGLE_PROCESS_MODE)
    d->forceSingleProcess = forceSingleProcess;
    d->waylandSocketName = waylandSocketName;
#else
    Q_UNUSED(forceSingleProcess)
    Q_UNUSED(waylandSocketName)
#endif

    d->roleNames.insert(Id, "applicationId");
    d->roleNames.insert(WindowItem, "windowItem");
    d->roleNames.insert(IsFullscreen, "isFullscreen");
    d->roleNames.insert(IsMapped, "isMapped");

    d->watchdogEnabled = true;

    connect(SystemMonitor::instance(), &SystemMonitor::fpsReportingEnabledChanged, this, [this]() {
        if (SystemMonitor::instance()->isFpsReportingEnabled()) {
            foreach (const QQuickWindow *view, d->views)
                connect(view, &QQuickWindow::frameSwapped, this, &WindowManager::reportFps);
        } else {
            foreach (const QQuickWindow *view, d->views)
                disconnect(view, &QQuickWindow::frameSwapped, this, &WindowManager::reportFps);
        }
    });
}

WindowManager::~WindowManager()
{
#if !defined(AM_SINGLE_PROCESS_MODE)
    delete d->waylandCompositor;
#endif
    delete d;
}

void WindowManager::enableWatchdog(bool enable)
{
    d->watchdogEnabled = enable;
}

bool WindowManager::isWatchdogEnabled() const
{
    return d->watchdogEnabled;
}

QVector<const Window *> WindowManager::windows() const
{
    // ugly cast to cheaply convert the pointer in the list to const
    return *(reinterpret_cast<QVector<const Window *> *>(&d->windows));
}

int WindowManager::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return d->windows.count();
}

QVariant WindowManager::data(const QModelIndex &index, int role) const
{
    if (index.parent().isValid() || !index.isValid())
        return QVariant();

    const Window *win = d->windows.at(index.row());

    switch (role) {
    case Id:
        if (win->application()) {
            if (win->application()->nonAliased())
                return win->application()->nonAliased()->id();
            else
                return win->application()->id();
        } else {
            return QString();
        }
    case WindowItem:
        return QVariant::fromValue<QQuickItem*>(win->windowItem());
    case IsFullscreen: {
        //TODO: find a way to handle fullscreen transparently for wayland and in-process
        return false;
    }
    case IsMapped: {
        if (win->isInProcess()) {
            return true;
        } else {
#ifndef AM_SINGLE_PROCESS_MODE
            auto ww = qobject_cast<const WaylandWindow*>(win);
            if (ww && ww->surface() && ww->surface()->surface())
                return ww->surface()->surface()->isMapped();
#endif
            return false;
        }
    }
    }
    return QVariant();
}

QHash<int, QByteArray> WindowManager::roleNames() const
{
    return d->roleNames;
}

/*!
    \qmlproperty int WindowManager::count

    This property holds the number of applications available.
*/
int WindowManager::count() const
{
    return rowCount();
}

/*!
    \qmlmethod object WindowManager::get(int index) const

    Retrieves the model data at \a index as a JavaScript object. Please see the \l {WindowManager
    Roles}{role names} for the expected object fields.

    Will return an empty object, if the specified \a index is invalid.
*/
QVariantMap WindowManager::get(int index) const
{
    if (index < 0 || index >= count()) {
        qCWarning(LogWayland) << "invalid index:" << index;
        return QVariantMap();
    }

    QVariantMap map;
    QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.begin(); it != roles.end(); ++it)
        map.insert(it.value(), data(QAbstractListModel::index(index), it.key()));
    return map;
}

void WindowManager::setupInProcessRuntime(AbstractRuntime *runtime)
{
    // special hacks to get in-process mode working transparently
    if (runtime->manager()->inProcess()) {
        QQmlEngine *e = qobject_cast<QQmlEngine*>(QObject::parent());

        runtime->setInProcessQmlEngine(e);

        connect(runtime, &AbstractRuntime::inProcessSurfaceItemFullscreenChanging,
                this, &WindowManager::surfaceFullscreenChanged, Qt::QueuedConnection);
        connect(runtime, &AbstractRuntime::inProcessSurfaceItemReady,
                this, static_cast<void (WindowManager::*)(QQuickItem *)>(&WindowManager::inProcessSurfaceItemCreated), Qt::QueuedConnection);
        connect(runtime, &AbstractRuntime::inProcessSurfaceItemClosing,
                this, &WindowManager::surfaceItemAboutToClose, Qt::QueuedConnection);
    }
}

/*!
    \qmlmethod WindowManager::releaseWindow(Item window)

    Releases all resources of the \a window surface.

    After cleanup is complete, the signal windowLost will be fired.
*/

void WindowManager::releaseWindow(QQuickItem *window)
{
    if (!window)
        return;
    window->deleteLater();
    surfaceItemDestroyed(window);
}

/*!
    \qmlmethod WindowManager::registerCompositorView(QQuickWindow *view)

    Register the given \a view as a possible destination for Wayland window composition.

    Wayland window items can only be rendered within top-level windows, that have been registered
    via this function.

    \note The \a view parameter is an actual top-level window on the server side - not a client
          application window item.
*/
void WindowManager::registerCompositorView(QQuickWindow *view)
{
    d->views << view;

#if !defined(AM_SINGLE_PROCESS_MODE)
    if (!d->forceSingleProcess) {
        if (!d->waylandCompositor) {
            d->waylandCompositor = new WaylandCompositor(view, d->waylandSocketName, this);
            connect(view, &QWindow::heightChanged, this, &WindowManager::resize);
            connect(view, &QWindow::widthChanged, this, &WindowManager::resize);
        } else {
            d->waylandCompositor->registerOutputWindow(view);
        }
    }
#endif
}

void WindowManager::surfaceFullscreenChanged(QQuickItem *surfaceItem, bool isFullscreen)
{
    Q_ASSERT(surfaceItem);

    int index = d->findWindowBySurfaceItem(surfaceItem);

    if (index == -1) {
        qCWarning(LogWayland) << "could not find an application window for surfaceItem";
        return;
    }

    QModelIndex modelIndex = QAbstractListModel::index(index);
    qCDebug(LogWayland) << "emitting dataChanged, index: " << modelIndex.row() << ", isFullScreen: " << isFullscreen;
    emit dataChanged(modelIndex, modelIndex, QVector<int>() << IsFullscreen);
}

/*! \internal
    Only used for in-process surfaces
 */
void WindowManager::inProcessSurfaceItemCreated(QQuickItem *surfaceItem)
{
    AbstractRuntime *rt = qobject_cast<AbstractRuntime *>(sender());
    if (!rt) {
        qCCritical(LogSystem) << "This function must be called by a signal of Runtime!";
        return;
    }
    const Application *app = rt->application();
    if (app->nonAliased())
        app = app->nonAliased();
    if (!app) {
        qCCritical(LogSystem) << "This function must be called by a signal of Runtime which actually has an application attached!";
        return;
    }

    setupWindow(new InProcessWindow(app, surfaceItem));
}


/*! \internal
    Used to create the Window objects for a surface
    This is called for both wayland and in-process surfaces
*/
void WindowManager::setupWindow(Window *window)
{
    if (!window)
        return;

    connect(window, &Window::windowPropertyChanged,
            this, [this](const QString &name, const QVariant &value) {
        if (Window *win = qobject_cast<Window *>(sender()))
            emit windowPropertyChanged(win->windowItem(), name, value);
    });

    beginInsertRows(QModelIndex(), d->windows.count(), d->windows.count());
    d->windows << window;
    endInsertRows();

    emit windowReady(d->windows.count() - 1, window->windowItem());
}

void WindowManager::surfaceItemDestroyed(QQuickItem* item)
{
    // item may already be deleted at this point!

    int index = d->findWindowBySurfaceItem(item);

    if (index > -1) {
        emit windowLost(index, item);

        beginRemoveRows(QModelIndex(), index, index);
        d->windows.removeAt(index);
        endRemoveRows();
    } else {
        qCCritical(LogSystem) << "ERROR: check why this code path has been entered! If that's ok, remove/change this debug output";
    }
}

void WindowManager::surfaceItemAboutToClose(QQuickItem *item)
{
    int index = d->findWindowBySurfaceItem(item);

    if (index == -1)
        qCWarning(LogSystem) << "could not find application for surfaceItem";
    emit windowClosing(index, item);  // is it really better to emit the signal with index==-1 then doing nothing...?
}


#if !defined(AM_SINGLE_PROCESS_MODE)

void WindowManager::resize()
{
#  if QT_VERSION < QT_VERSION_CHECK(5, 5, 0)
    d->waylandCompositor->setOutputGeometry(d->views.at(0)->geometry());
#  endif
}

void WindowManager::waylandSurfaceCreated(WindowSurface *surface)
{
    qCDebug(LogWayland) << "waylandSurfaceCreate" << surface->surface() << "(PID:" << surface->processId() << ")" << surface->windowProperties();

    connect(surface->surface(), &QWaylandSurface::surfaceDestroyed, this, &WindowManager::waylandSurfaceDestroyed);
}

void WindowManager::waylandSurfaceMapped(WindowSurface *surface)
{
    qCDebug(LogWayland) << "waylandSurfaceMapped" << surface->surface();
    Q_ASSERT(surface != 0);

    qint64 processId = surface->processId();
    if (processId == 0)
        return; //TODO: find out what those surfaces are and what I should do with them ;)

    const Application* app = ApplicationManager::instance()->fromProcessId(processId);

    if (!app && ApplicationManager::instance()->securityChecksEnabled()) {
        qCWarning(LogWayland) << "SECURITY ALERT: an unknown application tried to create a wayland-surface!";
        return;
    }

    Q_ASSERT(surface->item());

    //Only create a new Window if we don't have it already in the window list, as the user controls whether windows are removed or not
    int index = d->findWindowByWaylandSurface(surface->surface());
    if (index == -1) {
        WaylandWindow *w = new WaylandWindow(app, surface);
        setupWindow(w);
    } else {
        QModelIndex modelIndex = QAbstractListModel::index(index);
        qCDebug(LogWayland) << "emitting dataChanged, index: " << modelIndex.row() << ", isMapped: true";
        emit dataChanged(modelIndex, modelIndex, QVector<int>() << IsMapped);
        emit windowReady(index, d->windows.at(index)->windowItem());
    }

    // switch on Wayland ping/pong -- currently disabled, since it is a bit unstable
    //if (d->watchdogEnabled)
    //    w->enablePing(true);

    if (app) {
        //We only take focus for applications.
        surface->takeFocus(); // otherwise we will never get keyboard focus in the client
    }
}

void WindowManager::waylandSurfaceUnmapped(WindowSurface *surface)
{
    qCDebug(LogWayland) << "waylandSurfaceUnmapped" << surface->surface();

    int index = d->findWindowByWaylandSurface(surface->surface());

    if (index == -1) {
        qCWarning(LogWayland) << "could not find an application window for surfaceItem";
        return;
    }

    QModelIndex modelIndex = QAbstractListModel::index(index);
    qCDebug(LogWayland) << "emitting dataChanged, index: " << modelIndex.row() << ", isMapped: false";
    emit dataChanged(modelIndex, modelIndex, QVector<int>() << IsMapped);

    handleWaylandSurfaceDestroyedOrUnmapped(surface->surface());
}

void WindowManager::waylandSurfaceDestroyed()
{
    QWaylandSurface *surface = qobject_cast<QWaylandSurface *>(sender());

    qCDebug(LogWayland) << "waylandSurfaceDestroyed" << surface;
    int index = d->findWindowByWaylandSurface(surface);
    if (index < 0)
        return;
    WaylandWindow *win = qobject_cast<WaylandWindow *>(d->windows.at(index));
    if (!win)
        return;

    // switch off Wayland ping/pong
    if (d->watchdogEnabled)
        win->enablePing(false);

    handleWaylandSurfaceDestroyedOrUnmapped(surface);

    d->windows.removeAt(index);
    win->deleteLater();
}

void WindowManager::handleWaylandSurfaceDestroyedOrUnmapped(QWaylandSurface *surface)
{
    int index = d->findWindowByWaylandSurface(surface);
    if (index < 0)
        return;

    Window *win = d->windows.at(index);

    if (win->windowItem() && !win->isClosing()) {
        win->setClosing();
        surfaceItemAboutToClose(win->windowItem());
    }
}

#endif // !defined(AM_SINGLE_PROCESS_MODE)


/*!
    \qmlmethod bool WindowManager::setWindowProperty(Item window, string name, var value)

    Sets an application \a window's shared property identified by \a name to the given \a value.

    These properties are shared between the system-ui and the client application: in single-process
    mode simply via a QVariantMap; in multi-process mode via Qt's extended surface Wayland extension.
    Changes on either side are signalled on the other side via windowPropertyChanged.

    \sa windowProperty, windowProperties
*/
bool WindowManager::setWindowProperty(QQuickItem *window, const QString &name, const QVariant &value)
{
    int index = d->findWindowBySurfaceItem(window);

    if (index < 0)
        return false;

    Window *win = d->windows.at(index);
    return win->setWindowProperty(name, value);
}

/*!
    \qmlmethod var WindowManager::windowProperty(Item window, string name) const

    Returns the value of an application \a window's shared property identified by \a name.

    \sa setWindowProperty
*/
QVariant WindowManager::windowProperty(QQuickItem *window, const QString &name) const
{
    int index = d->findWindowBySurfaceItem(window);

    if (index < 0)
        return QVariant();

    const Window *win = d->windows.at(index);
    return win->windowProperty(name);
}

/*!
    \qmlmethod var WindowManager::windowProperties(Item window) const

    Returns an object containing all shared properties of an application \a window.

    \sa setWindowProperty
*/
QVariantMap WindowManager::windowProperties(QQuickItem *window) const
{
    int index = d->findWindowBySurfaceItem(window);

    if (index < 0)
        return QVariantMap();

    Window *win = d->windows.at(index);
    return win->windowProperties();
}

/*!
    \qmlsignal WindowManager::windowPropertyChanged(Item window, string name, var value)

    Reports a change of an application \a window's property identified by \a name to the given
    \a value.

    \sa setWindowProperty
*/

void WindowManager::reportFps()
{
    SystemMonitor::instance()->reportFrameSwap(nullptr);
}

bool WindowManager::setDBusPolicy(const QVariantMap &yamlFragment)
{
    static const QVector<QByteArray> functions {
        QT_STRINGIFY(makeScreenshot)
    };

    d->dbusPolicy = parseDBusPolicy(yamlFragment);

    foreach (const QByteArray &f, d->dbusPolicy.keys()) {
       if (!functions.contains(f))
           return false;
    }
    return true;
}

/*!
    \qmlmethod bool WindowManager::makeScreenshot(string filename, string selector)

    TODO
*/
bool WindowManager::makeScreenshot(const QString &filename, const QString &selector)
{
    AM_AUTHENTICATE_DBUS(bool)

    // filename:
    // %s -> screenId
    // %i -> appId
    // %% -> %

    // selector:
    // <appid>[attribute=value]:<screenid>
    // e.g. com.pelagicore.music[windowType=widget]:1
    //      com.pelagicore.*[windowType=]

    auto substituteFilename = [filename](const QString &screenId, const QString &appId) -> QString {
        QString result;
        bool percent = false;
        for (int i = 0; i < filename.size(); ++i) {
            QChar c = filename.at(i);
            if (!percent) {
                if (c != QLatin1Char('%'))
                    result.append(c);
                else
                    percent = true;
            } else {
                switch (c.unicode()) {
                case '%':
                    result.append(c);
                    break;
                case 's':
                    result.append(screenId);
                    break;
                case 'i':
                    result.append(appId);
                    break;
                }
                percent = false;
            }
        }
        return result;
    };

    QRegularExpression re(QStringLiteral("^([a-z.-]+)?(\\[([a-zA-Z0-9_.]+)=([^\\]]*)\\])?(:([0-9]+))?"));
    auto match = re.match(selector);
    QString screenId = match.captured(6);
    QString appId = match.captured(1);
    QString attributeName = match.captured(3);
    QString attributeValue = match.captured(4);

    // qWarning() << "Matching result:" << match.isValid();
    // qWarning() << "  screen ...... :" << screenId;
    // qWarning() << "  app ......... :" << appId;
    // qWarning() << "  attributeName :" << attributeName;
    // qWarning() << "  attributeValue:" << attributeValue;

    bool result = true;
    bool foundAtLeastOne = false;

    if (appId.isEmpty() && attributeName.isEmpty()) {
        // fullscreen screenshot

        for (int i = 0; i < d->views.count(); ++i) {
            if (screenId.isEmpty() || screenId.toInt() == i) {
                QImage img = d->views.at(i)->grabWindow();

                foundAtLeastOne = true;
                result &= img.save(substituteFilename(QString::number(i), QString()));
            }
        }
    } else {
        // app without system-UI

        QVector<const Application *> apps;
        foreach (const Application *app, ApplicationManager::instance()->applications()) {
            if (!app->isAlias() && (appId.isEmpty() || (appId == app->id())))
                apps << app;
        }

        auto grabbers = new QList<QSharedPointer<QQuickItemGrabResult>>;

        foreach (const Window *w, d->windows) {
            if (apps.contains(w->application())) {
                if (attributeName.isEmpty()
                        || (w->windowProperty(attributeName).toString() == attributeValue)) {
                    for (int i = 0; i < d->views.count(); ++i) {
                        if (screenId.isEmpty() || screenId.toInt() == i) {
                            QQuickWindow *view = d->views.at(i);

                            bool onScreen = false;

                            if (w->isInProcess()) {
                                onScreen = (w->windowItem()->window() == view);
                            }
#ifndef AM_SINGLE_PROCESS_MODE
                            else if (const WaylandWindow *wlw = qobject_cast<const WaylandWindow *>(w)) {
#  if QT_VERSION < QT_VERSION_CHECK(5, 5, 0)
                                Q_UNUSED(wlw)
                                onScreen = true;
#  else
                                onScreen = wlw->surface() && (wlw->surface()->outputWindow() == view);
#  endif
                            }
#endif
                            if (onScreen) {
                                foundAtLeastOne = true;
                                QSharedPointer<QQuickItemGrabResult> grabber = w->windowItem()->grabToImage();

                                if (!grabber) {
                                    result = false;
                                    continue;
                                }

                                QString saveTo = substituteFilename(QString::number(i), w->application()->id());
#if defined(QT_DBUS_LIB)
                                struct DBusDelayedReply
                                {
                                    DBusDelayedReply(const QString &connectionName)
                                        : dbusConn(connectionName), ok(true)
                                    { }
                                    QDBusMessage dbusReply;
                                    QDBusConnection dbusConn;
                                    bool ok;
                                } *dbusDelayedReply = nullptr;
                                if (calledFromDBus() && grabbers->isEmpty()) {
                                    setDelayedReply(true);
                                    dbusDelayedReply = new DBusDelayedReply(connection().name());
                                    dbusDelayedReply->dbusReply = message().createReply();
                                    dbusDelayedReply->ok = true;
                                }
#else
                                void *dbusDelayedReply = nullptr;
#endif
                                grabbers->append(grabber);
                                connect(grabber.data(), &QQuickItemGrabResult::ready, this, [grabbers, grabber, saveTo, dbusDelayedReply]() {
                                    bool ok = grabber->saveToFile(saveTo);
                                    grabbers->removeOne(grabber);
#if defined(QT_DBUS_LIB)
                                    if (dbusDelayedReply)
                                        dbusDelayedReply->ok &= ok;
#endif
                                    if (grabbers->isEmpty()) {
                                        delete grabbers;
#if defined(QT_DBUS_LIB)
                                        if (dbusDelayedReply) {
                                            dbusDelayedReply->dbusReply << QVariant(dbusDelayedReply->ok);
                                            dbusDelayedReply->dbusConn.send(dbusDelayedReply->dbusReply);
                                        }
#else
                                        Q_UNUSED(dbusDelayedReply)
#endif
                                    }
                                });

                            }
                        }
                    }
                }
            }
        }
    }
    return foundAtLeastOne & result;
}


int WindowManagerPrivate::findWindowByApplication(const Application *app) const
{
    for (int i = 0; i < windows.count(); ++i) {
        if (app == windows.at(i)->application())
            return i;
        if (app->nonAliased() && app->nonAliased() == windows.at(i)->application())
            return i;
    }
    return -1;
}

int WindowManagerPrivate::findWindowBySurfaceItem(QQuickItem *quickItem) const
{
    for (int i = 0; i < windows.count(); ++i) {
        if (quickItem == windows.at(i)->windowItem())
            return i;
    }
    return -1;
}

#if !defined(AM_SINGLE_PROCESS_MODE)

int WindowManagerPrivate::findWindowByWaylandSurface(QWaylandSurface *waylandSurface) const
{
    for (int i = 0; i < windows.count(); ++i) {
        if (!windows.at(i)->isInProcess() && (waylandSurface == waylandCompositor->waylandSurfaceFromItem(windows.at(i)->windowItem())))
            return i;
    }
    return -1;
}

#endif // !defined(AM_SINGLE_PROCESS_MODE)

