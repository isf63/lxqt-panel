/* BEGIN_COMMON_COPYRIGHT_HEADER
 * (c)LGPL2+
 *
 * LXQt - a lightweight, Qt based, desktop toolset
 * https://lxqt.org
 *
 * Copyright: 2010-2011 Razor team
 * Authors:
 *   Alexander Sokoloff <sokoloff.a@gmail.com>
 *
 * This program or library is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 *
 * END_COMMON_COPYRIGHT_HEADER */


#include "lxqtpanel.h"
#include "lxqtpanellimits.h"
#include "ilxqtpanelplugin.h"
#include "lxqtpanelapplication.h"
#include "lxqtpanellayout.h"
#include "config/configpaneldialog.h"
#include "popupmenu.h"
#include "plugin.h"
#include "panelpluginsmodel.h"
#include "windownotifier.h"
#include <LXQt/PluginInfo>

#include <QScreen>
#include <QWindow>
#include <QDebug>
#include <QString>
#include <QMenu>
#include <QMessageBox>
#include <QDropEvent>
#include <QPainter>
#include <XdgIcon>
#include <XdgDirs>

#include <KWindowSystem>
#include <KX11Extras>
#include <NETWM>

#include <algorithm>

#include "backends/ilxqtabstractwmiface.h"


#include <LayerShellQt/Window>

// Turn on this to show the time required to load each plugin during startup
// #define DEBUG_PLUGIN_LOADTIME
#ifdef DEBUG_PLUGIN_LOADTIME
#include <QElapsedTimer>
#endif

// Config keys and groups
#define CFG_KEY_SCREENNUM          "desktop"
#define CFG_KEY_POSITION           "position"
#define CFG_KEY_PANELSIZE          "panelSize"
#define CFG_KEY_ICONSIZE           "iconSize"
#define CFG_KEY_LINECNT            "lineCount"
#define CFG_KEY_LENGTH             "width"
#define CFG_KEY_PERCENT            "width-percent"
#define CFG_KEY_ALIGNMENT          "alignment"
#define CFG_KEY_FONTCOLOR          "font-color"
#define CFG_KEY_BACKGROUNDCOLOR    "background-color"
#define CFG_KEY_BACKGROUNDIMAGE    "background-image"
#define CFG_KEY_OPACITY            "opacity"
#define CFG_KEY_RESERVESPACE       "reserve-space"
#define CFG_KEY_PLUGINS            "plugins"
#define CFG_KEY_HIDABLE            "hidable"
#define CFG_KEY_VISIBLE_MARGIN     "visible-margin"
#define CFG_KEY_HIDE_ON_OVERLAP    "hide-on-overlap"
#define CFG_KEY_ANIMATION          "animation-duration"
#define CFG_KEY_SHOW_DELAY         "show-delay"
#define CFG_KEY_LOCKPANEL          "lockPanel"

/************************************************
 Returns the Position by the string.
 String is one of "Top", "Left", "Bottom", "Right", string is not case sensitive.
 If the string is not correct, returns defaultValue.
 ************************************************/
ILXQtPanel::Position LXQtPanel::strToPosition(const QString& str, ILXQtPanel::Position defaultValue)
{
    if (str.toUpper() == QLatin1String("TOP"))    return LXQtPanel::PositionTop;
    if (str.toUpper() == QLatin1String("LEFT"))   return LXQtPanel::PositionLeft;
    if (str.toUpper() == QLatin1String("RIGHT"))  return LXQtPanel::PositionRight;
    if (str.toUpper() == QLatin1String("BOTTOM")) return LXQtPanel::PositionBottom;
    return defaultValue;
}


/************************************************
 Return  string representation of the position
 ************************************************/
QString LXQtPanel::positionToStr(ILXQtPanel::Position position)
{
    switch (position)
    {
    case LXQtPanel::PositionTop:
        return QStringLiteral("Top");
    case LXQtPanel::PositionLeft:
        return QStringLiteral("Left");
    case LXQtPanel::PositionRight:
        return QStringLiteral("Right");
    case LXQtPanel::PositionBottom:
        return QStringLiteral("Bottom");
    }

    return QString();
}


/************************************************

 ************************************************/
LXQtPanel::LXQtPanel(const QString &configGroup, LXQt::Settings *settings, QWidget *parent) :
    QFrame(parent),
    mSettings(settings),
    mConfigGroup(configGroup),
    mPlugins{nullptr},
    mStandaloneWindows{new WindowNotifier},
    mPanelSize(0),
    mIconSize(0),
    mLineCount(0),
    mLength(0),
    mAlignment(AlignmentLeft),
    mPosition(ILXQtPanel::PositionBottom),
    mScreenNum(0), //whatever (avoid conditional on uninitialized value)
    mActualScreenNum(0),
    mWaylandScreenNum(-1), // not set yet (on Wayland)
    mHidable(false),
    mVisibleMargin(true),
    mHideOnOverlap(false),
    mHidden(false),
    mAnimationTime(0),
    mReserveSpace(true),
    mAnimation(nullptr),
    mWAnimation(nullptr),
    mLayerWindow(nullptr),
    mLockPanel(false)
{
    //You can find information about the flags and widget attributes in your
    //Qt documentation or at https://doc.qt.io/qt-5/qt.html
    //Qt::FramelessWindowHint = Produces a borderless window. The user cannot
    //move or resize a borderless window via the window system. On X11, ...
    Qt::WindowFlags flags = Qt::FramelessWindowHint;

    // NOTE: by PCMan:
    // In Qt 4, the window is not activated if it has Qt::WA_X11NetWmWindowTypeDock.
    // Since Qt 5, the default behaviour is changed. A window is always activated on mouse click.
    // Please see the source code of Qt5: src/plugins/platforms/xcb/qxcbwindow.cpp.
    // void QXcbWindow::handleButtonPressEvent(const xcb_button_press_event_t *event)
    // This new behaviour caused lxqt bug #161 - Cannot minimize windows from panel 1 when two task managers are open
    // Besides, this breaks minimizing or restoring windows when clicking on the taskbar buttons.
    // To workaround this regression bug, we need to add this window flag here.
    // However, since the panel gets no keyboard focus, this may decrease accessibility since
    // it's not possible to use the panel with keyboards. We need to find a better solution later.
    flags |= Qt::WindowDoesNotAcceptFocus;

    setWindowFlags(flags);
    //Adds _NET_WM_WINDOW_TYPE_DOCK to the window's _NET_WM_WINDOW_TYPE X11 window property. See https://standards.freedesktop.org/wm-spec/ for more details.
    setAttribute(Qt::WA_X11NetWmWindowTypeDock);
    //Enables tooltips for inactive windows.
    setAttribute(Qt::WA_AlwaysShowToolTips);
    //Indicates that the widget should have a translucent background, i.e., any non-opaque regions of the widgets will be translucent because the widget will have an alpha channel. Setting this ...
    setAttribute(Qt::WA_TranslucentBackground);
    //Allows data from drag and drop operations to be dropped onto the widget (see QWidget::setAcceptDrops()).
    setAttribute(Qt::WA_AcceptDrops);

    setWindowTitle(QStringLiteral("LXQt Panel"));
    setObjectName(QStringLiteral("LXQtPanel %1").arg(configGroup));

    //LXQtPanel (inherits QFrame) -> lav (QGridLayout) -> LXQtPanelWidget (QFrame) -> LXQtPanelLayout
    LXQtPanelWidget = new QFrame(this);
    LXQtPanelWidget->setObjectName(QStringLiteral("BackgroundWidget"));
    QGridLayout* lav = new QGridLayout();
    lav->setContentsMargins(0, 0, 0, 0);
    setLayout(lav);
    this->layout()->addWidget(LXQtPanelWidget);

    mLayout = new LXQtPanelLayout(LXQtPanelWidget);
    connect(mLayout, &LXQtPanelLayout::pluginMoved, this, &LXQtPanel::pluginMoved);
    LXQtPanelWidget->setLayout(mLayout);
    mLayout->setLineCount(mLineCount);

    mDelaySave.setSingleShot(true);
    mDelaySave.setInterval(SETTINGS_SAVE_DELAY);
    connect(&mDelaySave, &QTimer::timeout, this, [this] { saveSettings(); } );

    mHideTimer.setSingleShot(true);
    mHideTimer.setInterval(PANEL_HIDE_DELAY);
    connect(&mHideTimer, &QTimer::timeout, this, &LXQtPanel::hidePanelWork);

    mShowDelayTimer.setSingleShot(true);
    mShowDelayTimer.setInterval(PANEL_SHOW_DELAY);
    connect(&mShowDelayTimer, &QTimer::timeout, this, [this] { showPanel(mAnimationTime > 0); });

    // screen updates
    connect(qApp, &QApplication::screenAdded, this, [this] (QScreen* newScreen) {
        connect(newScreen, &QScreen::virtualGeometryChanged, this, &LXQtPanel::ensureVisible);
        connect(newScreen, &QScreen::geometryChanged, this, &LXQtPanel::ensureVisible);
        ensureVisible();
    });
    connect(qApp, &QApplication::screenRemoved, this, [this] (QScreen* oldScreen) {
        disconnect(oldScreen, &QScreen::virtualGeometryChanged, this, &LXQtPanel::ensureVisible);
        disconnect(oldScreen, &QScreen::geometryChanged, this, &LXQtPanel::ensureVisible);
        if (QGuiApplication::platformName() != QStringLiteral("wayland"))
        {
            // wait until the screen is really removed because it may contain the panel
            QTimer::singleShot(0, this, &LXQtPanel::ensureVisible);
        }
    });
    const auto screens = QApplication::screens();
    for(const auto& screen : screens)
    {
        connect(screen, &QScreen::virtualGeometryChanged, this, &LXQtPanel::ensureVisible);
        connect(screen, &QScreen::geometryChanged, this, &LXQtPanel::ensureVisible);
    }

    connect(LXQt::Settings::globalSettings(), &LXQt::GlobalSettings::settingsChanged, this, [this] { update(); } );
    connect(lxqtApp,                          &LXQt::Application::themeChanged,       this, &LXQtPanel::realign);

    connect(mStandaloneWindows.get(), &WindowNotifier::firstShown, this, [this] { showPanel(true); });
    connect(mStandaloneWindows.get(), &WindowNotifier::lastHidden, this, &LXQtPanel::hidePanel);

    readSettings();

    ensureVisible();

    loadPlugins();

    if(qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
    {
        // Create backing QWindow for LayerShellQt integration
        create();

        if(!windowHandle())
        {
            qWarning() << "LXQtPanel: could not create QWindow for LayerShellQt integration.";
        }
        else
        {
            // Init Layer Shell (Must be done before showing widget)
            mLayerWindow = LayerShellQt::Window::get(windowHandle());
            mLayerWindow->setLayer(LayerShellQt::Window::LayerTop);

            mLayerWindow->setScope(QStringLiteral("dock"));

            LayerShellQt::Window::Anchors anchors;
            anchors.setFlag(LayerShellQt::Window::AnchorLeft);
            anchors.setFlag(LayerShellQt::Window::AnchorBottom);
            anchors.setFlag(LayerShellQt::Window::AnchorRight);
            mLayerWindow->setAnchors(anchors);

#if (QT_VERSION >= QT_VERSION_CHECK(6,8,0))
            // WARNING: Only the following desktops are known to give the focus to child popups
            // when the panel does not accept focus.
            const QRegularExpression desktops(QStringLiteral("(?i)(kde|kwin|wayfire|hyprland)"));

            if (desktops.match(qEnvironmentVariable("XDG_CURRENT_DESKTOP")).hasMatch())
                mLayerWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
            else
#endif
                mLayerWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);

            mLayerWindow->setCloseOnDismissed(false);

            mLayerWindow->setExclusiveEdge(LayerShellQt::Window::AnchorBottom);
            mLayerWindow->setExclusiveZone(height());
        }
    }

    // NOTE: Some (X11) WMs may need the geometry to be set before QWidget::show().
    setPanelGeometry();

    show();

    // show it the first time, despite setting
    if (mHidable)
    {
        showPanel(false);
        QTimer::singleShot(PANEL_HIDE_FIRST_TIME, this, SLOT(hidePanel()));
    }

    LXQtPanelApplication *a = reinterpret_cast<LXQtPanelApplication*>(qApp);
    auto wmBackend = a->getWMBackend();

    connect(wmBackend, &ILXQtAbstractWMInterface::windowAdded, this, [this] {
        if (mHidable && mHideOnOverlap && !mHidden)
        {
            mShowDelayTimer.stop();
            hidePanel();
        }
    });
    connect(wmBackend, &ILXQtAbstractWMInterface::windowRemoved, this, [this] {
        if (mHidable && mHideOnOverlap && mHidden && !isPanelOverlapped())
            mShowDelayTimer.start();
    });
    connect(wmBackend, &ILXQtAbstractWMInterface::currentWorkspaceChanged, this, [this] {
       if (mHidable && mHideOnOverlap)
       {
            if (!mHidden)
            {
                mShowDelayTimer.stop();
                hidePanel();
            }
            else if (!isPanelOverlapped())
                mShowDelayTimer.start();
            else
                mShowDelayTimer.stop(); // workspace may be changed and restored quickly
       }
    });
    connect(wmBackend, &ILXQtAbstractWMInterface::windowPropertyChanged,
            this, [this] (WId /* id */, int prop)
    {
        if (mHidable && mHideOnOverlap
            // when a window is moved, resized, shaded, or minimized
            && (prop == int(LXQtTaskBarWindowProperty::Geometry)
                || prop == int(LXQtTaskBarWindowProperty::State)
                // on Wayland, workspace change is not seen as geometry change
                || (mLayerWindow && prop == int(LXQtTaskBarWindowProperty::Workspace))))
        {
            if (!mHidden)
            {
                mShowDelayTimer.stop();
                hidePanel();
            }
            else if (!isPanelOverlapped())
                mShowDelayTimer.start();
            else
                mShowDelayTimer.stop(); // geometry or state may be changed and restored quickly
        }
    });
}

/************************************************

 ************************************************/
void LXQtPanel::readSettings()
{
    // Read settings ......................................
    mSettings->beginGroup(mConfigGroup);

    // Let Hidability be the first thing we read
    // so that every call to realign() is without side-effect
    mHidable = mSettings->value(QStringLiteral(CFG_KEY_HIDABLE), mHidable).toBool();
    mHidden = mHidable;

    mVisibleMargin = mSettings->value(QStringLiteral(CFG_KEY_VISIBLE_MARGIN), mVisibleMargin).toBool();

    mHideOnOverlap = mSettings->value(QStringLiteral(CFG_KEY_HIDE_ON_OVERLAP), mHideOnOverlap).toBool();

    mAnimationTime = mSettings->value(QStringLiteral(CFG_KEY_ANIMATION), mAnimationTime).toInt();
    mShowDelayTimer.setInterval(mSettings->value(QStringLiteral(CFG_KEY_SHOW_DELAY), mShowDelayTimer.interval()).toInt());

    // By default we are using size & count from theme.
    setPanelSize(mSettings->value(QStringLiteral(CFG_KEY_PANELSIZE), PANEL_DEFAULT_SIZE).toInt(), false);
    setIconSize(mSettings->value(QStringLiteral(CFG_KEY_ICONSIZE), PANEL_DEFAULT_ICON_SIZE).toInt(), false);
    setLineCount(mSettings->value(QStringLiteral(CFG_KEY_LINECNT), PANEL_DEFAULT_LINE_COUNT).toInt(), false);

    setLength(mSettings->value(QStringLiteral(CFG_KEY_LENGTH), 100).toInt(),
              mSettings->value(QStringLiteral(CFG_KEY_PERCENT), true).toBool(),
              false);

    const auto screens = QApplication::screens();
    mScreenNum = std::clamp<int>(mSettings->value(QStringLiteral(CFG_KEY_SCREENNUM), 0).toInt(), 0, screens.size() - 1);
    if (QGuiApplication::platformName() == QStringLiteral("wayland"))
    {
        // On Wayland, first check the screen name, and if it does not exist, add it.
        mScreenName = mSettings->value(QStringLiteral(CFG_KEY_SCREENNAME)).toString();
        mWaylandScreenNum = mScreenNum;
        if (mScreenName.isEmpty())
        {
            mScreenName = screens.at(mWaylandScreenNum)->name();
            QTimer::singleShot(0, this, [this] { saveSettings(true); }); // save the found name
        }
        else
        {
            for (int i = 0; i < screens.size(); ++i)
            {
                if (screens.at(i)->name() == mScreenName)
                {
                    mWaylandScreenNum = i;
                    break;
                }
            }
        }
        setPosition(mWaylandScreenNum,
                    strToPosition(mSettings->value(QStringLiteral(CFG_KEY_POSITION)).toString(), PositionBottom),
                    false);
    }
    else
    {
        setPosition(mScreenNum,
                    strToPosition(mSettings->value(QStringLiteral(CFG_KEY_POSITION)).toString(), PositionBottom),
                    false);
    }

    setAlignment(Alignment(mSettings->value(QStringLiteral(CFG_KEY_ALIGNMENT), mAlignment).toInt()), false);

    QColor color = mSettings->value(QStringLiteral(CFG_KEY_FONTCOLOR), QString()).value<QColor>();
    if (color.isValid())
        setFontColor(color, false);

    setOpacity(mSettings->value(QStringLiteral(CFG_KEY_OPACITY), 100).toInt(), false);
    mReserveSpace = mSettings->value(QStringLiteral(CFG_KEY_RESERVESPACE), true).toBool();
    color = mSettings->value(QStringLiteral(CFG_KEY_BACKGROUNDCOLOR), QString()).value<QColor>();
    if (color.isValid())
        setBackgroundColor(color, false);

    QString image = mSettings->value(QStringLiteral(CFG_KEY_BACKGROUNDIMAGE), QString()).toString();
    if (!image.isEmpty())
        setBackgroundImage(image, false);

    mLockPanel = mSettings->value(QStringLiteral(CFG_KEY_LOCKPANEL), false).toBool();

    mSettings->endGroup();
}


/************************************************

 ************************************************/
void LXQtPanel::saveSettings(bool later)
{
    mDelaySave.stop();
    if (later)
    {
        mDelaySave.start();
        return;
    }

    mSettings->beginGroup(mConfigGroup);

    //Note: save/load of plugin names is completely handled by mPlugins object
    //mSettings->setValue(CFG_KEY_PLUGINS, mPlugins->pluginNames());

    mSettings->setValue(QStringLiteral(CFG_KEY_PANELSIZE), mPanelSize);
    mSettings->setValue(QStringLiteral(CFG_KEY_ICONSIZE), mIconSize);
    mSettings->setValue(QStringLiteral(CFG_KEY_LINECNT), mLineCount);

    mSettings->setValue(QStringLiteral(CFG_KEY_LENGTH), mLength);
    mSettings->setValue(QStringLiteral(CFG_KEY_PERCENT), mLengthInPercents);

    mSettings->setValue(QStringLiteral(CFG_KEY_SCREENNUM), mScreenNum);
    if (mWaylandScreenNum >= 0) // on Wayland
    {
        if (mScreenName.isEmpty())
        {
            const auto screens = QApplication::screens();
            if (mWaylandScreenNum < screens.size())
                mScreenName = screens.at(mWaylandScreenNum)->name();
        }
        mSettings->setValue(QStringLiteral(CFG_KEY_SCREENNAME), mScreenName);
    }
    mSettings->setValue(QStringLiteral(CFG_KEY_POSITION), positionToStr(mPosition));

    mSettings->setValue(QStringLiteral(CFG_KEY_ALIGNMENT), mAlignment);

    mSettings->setValue(QStringLiteral(CFG_KEY_FONTCOLOR), mFontColor.isValid() ? mFontColor : QColor());
    mSettings->setValue(QStringLiteral(CFG_KEY_BACKGROUNDCOLOR), mBackgroundColor.isValid() ? mBackgroundColor : QColor());
    mSettings->setValue(QStringLiteral(CFG_KEY_BACKGROUNDIMAGE), QFileInfo::exists(mBackgroundImage) ? mBackgroundImage : QString());
    mSettings->setValue(QStringLiteral(CFG_KEY_OPACITY), mOpacity);
    mSettings->setValue(QStringLiteral(CFG_KEY_RESERVESPACE), mReserveSpace);

    mSettings->setValue(QStringLiteral(CFG_KEY_HIDABLE), mHidable);
    mSettings->setValue(QStringLiteral(CFG_KEY_VISIBLE_MARGIN), mVisibleMargin);
    mSettings->setValue(QStringLiteral(CFG_KEY_HIDE_ON_OVERLAP), mHideOnOverlap);
    mSettings->setValue(QStringLiteral(CFG_KEY_ANIMATION), mAnimationTime);
    mSettings->setValue(QStringLiteral(CFG_KEY_SHOW_DELAY), mShowDelayTimer.interval());

    mSettings->setValue(QStringLiteral(CFG_KEY_LOCKPANEL), mLockPanel);

    mSettings->endGroup();
}


/************************************************

 ************************************************/
void LXQtPanel::ensureVisible()
{
    if (!mScreenName.isEmpty() && QGuiApplication::platformName() == QStringLiteral("wayland"))
    {
        // Find the Wayland screen number based on the screen name.
        mWaylandScreenNum = -1; // first unset it
        const auto screens = QApplication::screens();
        for (int i = 0; i < screens.size(); ++i)
        {
            if (screens.at(i)->name() == mScreenName)
            {
                mWaylandScreenNum = i;
                break;
            }
        }
        if (mWaylandScreenNum < 0)
            return;
        if (!canPlacedOn(mWaylandScreenNum, mPosition))
            setPosition(findAvailableScreen(mPosition), mPosition, false);
        else
            mActualScreenNum = mWaylandScreenNum;
    }
    else
    {
        if (!canPlacedOn(mScreenNum, mPosition))
            setPosition(findAvailableScreen(mPosition), mPosition, false);
        else
            mActualScreenNum = mScreenNum;
    }

    // the screen size might be changed
    realign();
}


/************************************************

 ************************************************/
LXQtPanel::~LXQtPanel()
{
    mLayout->setEnabled(false);
    delete mAnimation;
    delete mWAnimation;
    delete mConfigDialog.data();
    // do not save settings because of "user deleted panel" functionality saveSettings();
}


/************************************************

 ************************************************/
void LXQtPanel::show()
{
    QWidget::show();
    if(qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) //TODO: cache in bool isPlatformX11
        KX11Extras::setOnDesktop(effectiveWinId(), NET::OnAllDesktops);
}


/************************************************

 ************************************************/
QStringList pluginDesktopDirs()
{
    QStringList dirs;
    dirs << QString::fromLocal8Bit(qgetenv("LXQT_PANEL_PLUGINS_DIR")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    dirs << QStringLiteral("%1/%2").arg(XdgDirs::dataHome(), QStringLiteral("/lxqt/lxqt-panel"));
    dirs << QStringLiteral(PLUGIN_DESKTOPS_DIR);
    return dirs;
}


/************************************************

 ************************************************/
void LXQtPanel::loadPlugins()
{
    QString names_key(mConfigGroup);
    names_key += QLatin1Char('/');
    names_key += QLatin1String(CFG_KEY_PLUGINS);
    mPlugins.reset(new PanelPluginsModel(this, names_key, pluginDesktopDirs()));

    connect(mPlugins.get(), &PanelPluginsModel::pluginAdded, mLayout, &LXQtPanelLayout::addPlugin);
    connect(mPlugins.get(), &PanelPluginsModel::pluginMovedUp, mLayout, &LXQtPanelLayout::moveUpPlugin);
    //reemit signals
    connect(mPlugins.get(), &PanelPluginsModel::pluginAdded, this, &LXQtPanel::pluginAdded);
    connect(mPlugins.get(), &PanelPluginsModel::pluginRemoved, this, &LXQtPanel::pluginRemoved);

    const auto plugins = mPlugins->plugins();
    for (auto const & plugin : plugins)
    {
        mLayout->addPlugin(plugin);
        connect(plugin, &Plugin::dragLeft, this, [this] {
            mShowDelayTimer.stop();
            hidePanel();
        });
    }
}

/************************************************

 ************************************************/
int LXQtPanel::getReserveDimension()
{
    return mHidable ? PANEL_HIDE_SIZE : std::max(PANEL_MINIMUM_SIZE, mPanelSize);
}

QMargins LXQtPanel::layerWindowMargins()
{
    QMargins margins;
    if (!mHidden)
        return margins;
    int offset = PANEL_HIDE_SIZE - std::max(PANEL_MINIMUM_SIZE, mPanelSize); // negative
    if (isHorizontal())
    {
        if (mPosition == ILXQtPanel::PositionTop)
            margins = QMargins(0, offset, 0, 0);
        else
            margins = QMargins(0, 0, 0, offset);
    }
    else
    {
        if (mPosition == ILXQtPanel::PositionLeft)
            margins = QMargins(offset, 0, 0, 0);
        else
            margins = QMargins(0, 0, offset, 0);
    }
    return margins;
}

void LXQtPanel::setPanelGeometry(bool animate)
{
    const auto screens = QApplication::screens();
    if (mActualScreenNum >= screens.size())
        return;
    const QRect currentScreen = screens.at(mActualScreenNum)->geometry();

    QRect rect;
    LayerShellQt::Window::Anchors anchors;

    if (isHorizontal())
    {
        // Horiz panel ***************************
        rect.setHeight(std::max(PANEL_MINIMUM_SIZE, mPanelSize));
        if (mLengthInPercents)
            rect.setWidth(currentScreen.width() * mLength / 100.0);
        else
        {
            if (mLength <= 0)
                rect.setWidth(currentScreen.width() + mLength);
            else
                rect.setWidth(mLength);
        }

        rect.setWidth(std::max(rect.size().width(), mLayout->minimumSize().width()));

        // Horiz ......................
        switch (mAlignment)
        {
        case LXQtPanel::AlignmentLeft:
            anchors.setFlag(LayerShellQt::Window::AnchorLeft);
            rect.moveLeft(currentScreen.left());
            break;

        case LXQtPanel::AlignmentCenter:
            rect.moveCenter(currentScreen.center());
            break;

        case LXQtPanel::AlignmentRight:
            anchors.setFlag(LayerShellQt::Window::AnchorRight);
            rect.moveRight(currentScreen.right());
            break;
        }

        if(lengthInPercents() && mLength == 100)
        {
            //Fill all available width
            anchors.setFlag(LayerShellQt::Window::AnchorLeft);
            anchors.setFlag(LayerShellQt::Window::AnchorRight);
        }

        // Vert .......................
        if (mPosition == ILXQtPanel::PositionTop)
        {
            anchors.setFlag(LayerShellQt::Window::AnchorTop);

            if (mHidden)
                rect.moveBottom(currentScreen.top() + PANEL_HIDE_SIZE - 1);
            else
                rect.moveTop(currentScreen.top());
        }
        else
        {
            anchors.setFlag(LayerShellQt::Window::AnchorBottom);

            if (mHidden)
                rect.moveTop(currentScreen.bottom() - PANEL_HIDE_SIZE + 1);
            else
                rect.moveBottom(currentScreen.bottom());
        }
    }
    else
    {
        // Vert panel ***************************
        rect.setWidth(std::max(PANEL_MINIMUM_SIZE, mPanelSize));
        if (mLengthInPercents)
            rect.setHeight(currentScreen.height() * mLength / 100.0);
        else
        {
            if (mLength <= 0)
                rect.setHeight(currentScreen.height() + mLength);
            else
                rect.setHeight(mLength);
        }

        rect.setHeight(std::max(rect.size().height(), mLayout->minimumSize().height()));

        // Vert .......................
        switch (mAlignment)
        {
        case LXQtPanel::AlignmentLeft:
            anchors.setFlag(LayerShellQt::Window::AnchorTop);
            rect.moveTop(currentScreen.top());
            break;

        case LXQtPanel::AlignmentCenter:
            rect.moveCenter(currentScreen.center());
            break;

        case LXQtPanel::AlignmentRight:
            anchors.setFlag(LayerShellQt::Window::AnchorBottom);
            rect.moveBottom(currentScreen.bottom());
            break;
        }

        if(lengthInPercents() && mLength == 100)
        {
            //Fill all available width
            anchors.setFlag(LayerShellQt::Window::AnchorTop);
            anchors.setFlag(LayerShellQt::Window::AnchorBottom);
        }

        // Horiz ......................
        if (mPosition == ILXQtPanel::PositionLeft)
        {
            anchors.setFlag(LayerShellQt::Window::AnchorLeft);

            if (mHidden)
                rect.moveRight(currentScreen.left() + PANEL_HIDE_SIZE - 1);
            else
                rect.moveLeft(currentScreen.left());
        }
        else
        {
            anchors.setFlag(LayerShellQt::Window::AnchorRight);

            if (mHidden)
                rect.moveLeft(currentScreen.right() - PANEL_HIDE_SIZE + 1);
            else
                rect.moveRight(currentScreen.right());
        }
    }

    if (!mHidden || !mGeometry.isValid()) mGeometry = rect;
    if (mLayerWindow)
    {
        // NOTE: On Wayland, QVariantAnimation is used to set appropriate negative margins.
        auto screen = screens.at(mActualScreenNum);
        if (screen != windowHandle()->screen())
        {
            // WARNING: An already visible window is not shown on a new screen under Wayland.
            if (isVisible())
            {
                hide();
                QTimer::singleShot(0, this, &QWidget::show);
            }
            windowHandle()->setScreen(screen);
        }
        mLayerWindow->setAnchors(anchors);
        setFixedSize(rect.size());
        if (animate)
        {
            if (mWAnimation == nullptr)
            {
                mWAnimation = new QVariantAnimation(this);
                mWAnimation->setEasingCurve(QEasingCurve::Linear);
                mWAnimation->setStartValue(static_cast<qreal>(0));
                mWAnimation->setEndValue(static_cast<qreal>(1));
                connect(mWAnimation, &QVariantAnimation::finished, this, [this] {
                    if (mHidden)
                    {
                        setMargins();
                        // "setWindowOpacity()" does not work on Wayland
                        if (!mVisibleMargin)
                            LXQtPanelWidget->setVisible(false);
                    }
                });
                connect(mWAnimation, &QVariantAnimation::valueChanged, this,
                        [this] (const QVariant &value) {
                    QMargins margins = layerWindowMargins();
                    QMarginsF m((mWAnimation->endValue().toReal() - value.toReal())
                                * mLayerWindow->margins().toMarginsF()
                                + value.toReal() * margins.toMarginsF());
                    mLayerWindow->setMargins(m.toMargins());
                    windowHandle()->requestUpdate();
                });
            }
            mWAnimation->setDuration(mAnimationTime);
            if (!mHidden)
            {
                setMargins();
                if (!mVisibleMargin)
                    LXQtPanelWidget->setVisible(true);
            }
            mWAnimation->start();
        }
        else
        {
            if (!mVisibleMargin)
                LXQtPanelWidget->setVisible(!mHidden);
            setMargins();
            mLayerWindow->setMargins(layerWindowMargins());
            windowHandle()->requestUpdate();
        }
    }
    else if (rect != geometry())
    {
        setFixedSize(rect.size());
        if (animate)
        {
            if (mAnimation == nullptr)
            {
                mAnimation = new QPropertyAnimation(this, "geometry");
                mAnimation->setEasingCurve(QEasingCurve::Linear);
                //Note: for hiding, the margins are set after animation is finished
                connect(mAnimation, &QAbstractAnimation::finished, this, [this] { if (mHidden) setMargins(); });
            }
            mAnimation->setDuration(mAnimationTime);
            mAnimation->setStartValue(geometry());
            mAnimation->setEndValue(rect);
            //Note: for showing-up, the margins are removed instantly
            if (!mHidden)
                setMargins();
            mAnimation->start();
        }
        else
        {
            setMargins();
            setGeometry(rect);
        }
    }
}

void LXQtPanel::setMargins()
{
    if (mHidden)
    {
        if (isHorizontal())
        {
            if (mPosition == ILXQtPanel::PositionTop)
                mLayout->setContentsMargins(0, 0, 0, PANEL_HIDE_SIZE);
            else
                mLayout->setContentsMargins(0, PANEL_HIDE_SIZE, 0, 0);
        }
        else
        {
            if (mPosition == ILXQtPanel::PositionLeft)
                mLayout->setContentsMargins(0, 0, PANEL_HIDE_SIZE, 0);
            else
                mLayout->setContentsMargins(PANEL_HIDE_SIZE, 0, 0, 0);
        }
        if (!mVisibleMargin)
            setWindowOpacity(0.0);
    }
    else {
        mLayout->setContentsMargins(0, 0, 0, 0);
        if (!mVisibleMargin)
            setWindowOpacity(1.0);
    }
}

void LXQtPanel::realign()
{
    if (!isVisible())
        return;
#if 0
    qDebug() << "** Realign *********************";
    qDebug() << "PanelSize:   " << mPanelSize;
    qDebug() << "IconSize:      " << mIconSize;
    qDebug() << "LineCount:     " << mLineCount;
    qDebug() << "Length:        " << mLength << (mLengthInPercents ? "%" : "px");
    qDebug() << "Alignment:     " << (mAlignment == 0 ? "center" : (mAlignment < 0 ? "left" : "right"));
    qDebug() << "Position:      " << positionToStr(mPosition) << "on" << mScreenNum;
    qDebug() << "Plugins count: " << mPlugins.count();
#endif

    setPanelGeometry();

    // Reserve our space on the screen ..........
    // It's possible that our geometry is not changed, but screen resolution is changed,
    // so resetting WM_STRUT is still needed. To make it simple, we always do it.
    updateWmStrut();
}


// Update the _NET_WM_PARTIAL_STRUT and _NET_WM_STRUT properties for the window
void LXQtPanel::updateWmStrut()
{
    WId wid = effectiveWinId();
    if(wid == 0 || !isVisible())
        return;

    if(qGuiApp->nativeInterface<QNativeInterface::QX11Application>())
    {
        if (mReserveSpace && QApplication::primaryScreen())
        {
            const QRect wholeScreen = QApplication::primaryScreen()->virtualGeometry();
            const QRect rect = geometry();
            // NOTE: https://standards.freedesktop.org/wm-spec/wm-spec-latest.html
            // Quote from the EWMH spec: " Note that the strut is relative to the screen edge, and not the edge of the xinerama monitor."
            // So, we use the geometry of the whole screen to calculate the strut rather than using the geometry of individual monitors.
            // Though the spec only mention Xinerama and did not mention XRandR, the rule should still be applied.
            // At least openbox is implemented like this.
            switch (mPosition)
            {
            case LXQtPanel::PositionTop:
                KX11Extras::setExtendedStrut(wid,
                                             /* Left   */  0, 0, 0,
                                             /* Right  */  0, 0, 0,
                                             /* Top    */  rect.top() + getReserveDimension(), rect.left(), rect.right(),
                                             /* Bottom */  0, 0, 0
                                             );
                break;

            case LXQtPanel::PositionBottom:
                KX11Extras::setExtendedStrut(wid,
                                             /* Left   */  0, 0, 0,
                                             /* Right  */  0, 0, 0,
                                             /* Top    */  0, 0, 0,
                                             /* Bottom */  wholeScreen.bottom() - rect.bottom() + getReserveDimension(), rect.left(), rect.right()
                                             );
                break;

            case LXQtPanel::PositionLeft:
                KX11Extras::setExtendedStrut(wid,
                                             /* Left   */  rect.left() + getReserveDimension(), rect.top(), rect.bottom(),
                                             /* Right  */  0, 0, 0,
                                             /* Top    */  0, 0, 0,
                                             /* Bottom */  0, 0, 0
                                             );

                break;

            case LXQtPanel::PositionRight:
                KX11Extras::setExtendedStrut(wid,
                                             /* Left   */  0, 0, 0,
                                             /* Right  */  wholeScreen.right() - rect.right() + getReserveDimension(), rect.top(), rect.bottom(),
                                             /* Top    */  0, 0, 0,
                                             /* Bottom */  0, 0, 0
                                             );
                break;
            }
        } else
        {
            KX11Extras::setExtendedStrut(wid,
                                         /* Left   */  0, 0, 0,
                                         /* Right  */  0, 0, 0,
                                         /* Top    */  0, 0, 0,
                                         /* Bottom */  0, 0, 0
                                         );
        }
    }
    else if(mLayerWindow && qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
    {
        if (mReserveSpace
            // NOTE: For some reason, no space is reserved with a negative layer margin.
            // However, there is no reason to reserve space for a hiding panel on Wayland.
            && !mHidable)
        {
            LayerShellQt::Window::Anchor edge = LayerShellQt::Window::AnchorBottom;

            switch (mPosition)
            {
            case LXQtPanel::PositionTop:
                edge = LayerShellQt::Window::AnchorTop;
                break;

            case LXQtPanel::PositionBottom:
                edge = LayerShellQt::Window::AnchorBottom;
                break;

            case LXQtPanel::PositionLeft:
                edge = LayerShellQt::Window::AnchorLeft;
                break;

            case LXQtPanel::PositionRight:
                edge = LayerShellQt::Window::AnchorRight;
                break;
            }

            mLayerWindow->setExclusiveEdge(edge);
            mLayerWindow->setExclusiveZone(getReserveDimension());
        }
        else
        {
            mLayerWindow->setExclusiveEdge(LayerShellQt::Window::AnchorNone);
            mLayerWindow->setExclusiveZone(0);
        }

        // Make LayerShellQt apply changes immediatly
        windowHandle()->requestUpdate();
    }
}


/************************************************
  This function checks if the panel can be placed on
  the display @screenNum at @position.
  NOTE: The panel can be placed only at screen edges
  but no part of it should be between two screens.
 ************************************************/
bool LXQtPanel::canPlacedOn(int screenNum, LXQtPanel::Position position)
{
    const auto screens = QApplication::screens();
    if (screens.size() > screenNum)
    {
        const QRect screenGeometry = screens.at(screenNum)->geometry();
        switch (position)
        {
        case LXQtPanel::PositionTop:
            for (const auto& screen : screens)
            {
                if (screen->geometry().top() < screenGeometry.top())
                {
                    QRect r = screenGeometry.adjusted(0, screen->geometry().top() - screenGeometry.top(), 0, 0);
                    if (screen->geometry().intersects(r))
                        return false;
                }
            }
            return true;

        case LXQtPanel::PositionBottom:
            for (const auto& screen : screens)
            {
                if (screen->geometry().bottom() > screenGeometry.bottom())
                {
                    QRect r = screenGeometry.adjusted(0, 0, 0, screen->geometry().bottom() - screenGeometry.bottom());
                    if (screen->geometry().intersects(r))
                        return false;
                }
            }
            return true;

        case LXQtPanel::PositionLeft:
            for (const auto& screen : screens)
            {
                if (screen->geometry().left() < screenGeometry.left())
                {
                    QRect r = screenGeometry.adjusted(screen->geometry().left() - screenGeometry.left(), 0, 0, 0);
                    if (screen->geometry().intersects(r))
                        return false;
                }
            }
            return true;

        case LXQtPanel::PositionRight:
            for (const auto& screen : screens)
            {
                if (screen->geometry().right() > screenGeometry.right())
                {
                    QRect r = screenGeometry.adjusted(0, 0, screen->geometry().right() - screenGeometry.right(), 0);
                    if (screen->geometry().intersects(r))
                        return false;
                }
            }
            return true;
        }
    }

    return false;
}


/************************************************

 ************************************************/
int LXQtPanel::findAvailableScreen(LXQtPanel::Position position)
{
    int current = (mWaylandScreenNum >= 0 ? mWaylandScreenNum // on Wayland
                                          : mScreenNum);

    for (int i = current; i < QApplication::screens().size(); ++i)
        if (canPlacedOn(i, position))
            return i;

    for (int i = 0; i < current; ++i)
        if (canPlacedOn(i, position))
            return i;

    return 0;
}


/************************************************

 ************************************************/
void LXQtPanel::showConfigDialog()
{
    if (mConfigDialog.isNull())
        mConfigDialog = new ConfigPanelDialog(this, nullptr /*make it top level window*/);

    mConfigDialog->showConfigPlacementPage();
    mStandaloneWindows->observeWindow(mConfigDialog.data());
    mConfigDialog->show();
    mConfigDialog->raise();
    mConfigDialog->activateWindow();
    WId wid = mConfigDialog->windowHandle()->winId();

    KX11Extras::activateWindow(wid);
    KX11Extras::setOnDesktop(wid, KX11Extras::currentDesktop());
}


/************************************************

 ************************************************/
void LXQtPanel::showAddPluginDialog()
{
    if (mConfigDialog.isNull())
        mConfigDialog = new ConfigPanelDialog(this, nullptr /*make it top level window*/);

    mConfigDialog->showConfigPluginsPage();
    mStandaloneWindows->observeWindow(mConfigDialog.data());
    mConfigDialog->show();
    mConfigDialog->raise();
    mConfigDialog->activateWindow();
    WId wid = mConfigDialog->windowHandle()->winId();

    KX11Extras::activateWindow(wid);
    KX11Extras::setOnDesktop(wid, KX11Extras::currentDesktop());
}


/************************************************

 ************************************************/
void LXQtPanel::updateStyleSheet()
{
    // NOTE: This is a workaround for Qt >= 5.13, which might not completely
    // update the style sheet (especially positioned backgrounds of plugins
    // with NeedsHandle="true") if it is not reset first.
    setStyleSheet(QString());

    QStringList sheet;
    sheet << QStringLiteral("Plugin > QAbstractButton, LXQtTray { qproperty-iconSize: %1px %1px; }").arg(mIconSize);
    sheet << QStringLiteral("Plugin > * > QAbstractButton, TrayIcon { qproperty-iconSize: %1px %1px; }").arg(mIconSize);

    if (mFontColor.isValid())
        sheet << QString(QStringLiteral("Plugin * { color: ") + mFontColor.name() + QStringLiteral("; }"));

    if (mBackgroundColor.isValid())
    {
        QString color = QStringLiteral("%1, %2, %3, %4")
            .arg(mBackgroundColor.red())
            .arg(mBackgroundColor.green())
            .arg(mBackgroundColor.blue())
            .arg((float) mOpacity / 100);
        sheet << QString(QStringLiteral("LXQtPanel #BackgroundWidget { background-color: rgba(") + color + QStringLiteral("); }"));
    }

    if (QFileInfo::exists(mBackgroundImage))
        sheet << QString(QStringLiteral("LXQtPanel #BackgroundWidget { background-image: url('") + mBackgroundImage + QStringLiteral("');}"));

    setStyleSheet(sheet.join(QStringLiteral("\n")));
}



/************************************************

 ************************************************/
void LXQtPanel::setPanelSize(int value, bool save)
{
    if (mPanelSize != value)
    {
        mPanelSize = value;
        realign();

        if (save)
            saveSettings(true);
    }
}



/************************************************

 ************************************************/
void LXQtPanel::setIconSize(int value, bool save)
{
    if (mIconSize != value)
    {
        mIconSize = value;
        updateStyleSheet();
        mLayout->setLineSize(mIconSize);

        if (save)
            saveSettings(true);

        realign();
    }
}


/************************************************

 ************************************************/
void LXQtPanel::setLineCount(int value, bool save)
{
    if (mLineCount != value)
    {
        mLineCount = value;
        mLayout->setEnabled(false);
        mLayout->setLineCount(mLineCount);
        mLayout->setEnabled(true);

        if (save)
            saveSettings(true);

        realign();
    }
}


/************************************************

 ************************************************/
void LXQtPanel::setLength(int length, bool inPercents, bool save)
{
    if (mLength == length &&
            mLengthInPercents == inPercents)
        return;

    mLength = length;
    mLengthInPercents = inPercents;

    if (save)
        saveSettings(true);

    realign();
}


/************************************************

 ************************************************/
void LXQtPanel::setPosition(int screen, ILXQtPanel::Position position, bool save)
{
    if ((mWaylandScreenNum >= 0 ? mWaylandScreenNum // on Wayland
                                : mScreenNum) == screen
        && mPosition == position)
        return;

    mActualScreenNum = screen;
    mPosition = position;
    mLayout->setPosition(mPosition);

    if (save)
    {
        if (mWaylandScreenNum >= 0)
        {
            mWaylandScreenNum = screen;
            mScreenName = qApp->screens().at(screen)->name();
        }
        else
            mScreenNum = screen;
        saveSettings(true);
    }

    // Qt 5 adds a new class QScreen and add API for setting the screen of a QWindow.
    // so we had better use it. However, without this, our program should still work
    // as long as XRandR is used. Since XRandR combined all screens into a large virtual desktop
    // every screen and their virtual siblings are actually on the same virtual desktop.
    // So things still work if we don't set the screen correctly, but this is not the case
    // for other backends, such as the upcoming wayland support. Hence it's better to set it.
    if(windowHandle())
    {
        // QScreen* newScreen = qApp->screens().at(screen);
        // QScreen* oldScreen = windowHandle()->screen();
        // const bool shouldRecreate = windowHandle()->handle() && !(oldScreen && oldScreen->virtualSiblings().contains(newScreen));
        // Q_ASSERT(shouldRecreate == false);

        // NOTE: When you move a window to another screen, Qt 5 might recreate the window as needed
        // But luckily, this never happen in XRandR, so Qt bug #40681 is not triggered here.
        // (The only exception is when the old screen is destroyed, Qt always re-create the window and
        // this corner case triggers #40681.)
        // When using other kind of multihead settings, such as Xinerama, this might be different and
        // unless Qt developers can fix their bug, we have no way to workaround that.
        if (mLayerWindow)
        {
            // WARNING: An already visible window is not shown on a new screen under Wayland.
            if (isVisible())
            {
                hide();
                QTimer::singleShot(0, this, &QWidget::show);
            }
        }
        windowHandle()->setScreen(qApp->screens().at(screen));
    }

    realign();
}

/************************************************
 *
 ************************************************/
void LXQtPanel::setAlignment(Alignment value, bool save)
{
    if (mAlignment == value)
        return;

    mAlignment = value;

    if (save)
        saveSettings(true);

    realign();
}

/************************************************
 *
 ************************************************/
void LXQtPanel::setFontColor(QColor color, bool save)
{
    mFontColor = color;
    updateStyleSheet();

    if (save)
        saveSettings(true);
}

/************************************************

 ************************************************/
void LXQtPanel::setBackgroundColor(QColor color, bool save)
{
    mBackgroundColor = color;
    updateStyleSheet();

    if (save)
        saveSettings(true);
}

/************************************************

 ************************************************/
void LXQtPanel::setBackgroundImage(QString path, bool save)
{
    mBackgroundImage = path;
    updateStyleSheet();

    if (save)
        saveSettings(true);
}


/************************************************
 *
 ************************************************/
void LXQtPanel::setOpacity(int opacity, bool save)
{
    mOpacity = std::clamp(opacity, 0, 100);
    updateStyleSheet();

    if (save)
        saveSettings(true);
}


/************************************************
 *
 ************************************************/
void LXQtPanel::setReserveSpace(bool reserveSpace, bool save)
{
    if (mReserveSpace == reserveSpace)
        return;

    mReserveSpace = reserveSpace;

    if (save)
        saveSettings(true);

    updateWmStrut();
}


/************************************************

 ************************************************/
QRect LXQtPanel::globalGeometry() const
{
    // panel is the the top-most widget/window, no calculation needed
    return geometry();
}


/************************************************

 ************************************************/
bool LXQtPanel::event(QEvent *event)
{
    switch (event->type())
    {
    case QEvent::ContextMenu:
        showPopupMenu(static_cast<QContextMenuEvent *>(event)->globalPos());
        break;

    case QEvent::LayoutRequest:
        emit realigned();
        break;

    case QEvent::WinIdChange:
    {
        if(qGuiApp->nativeInterface<QNativeInterface::QX11Application>())
        {
            // qDebug() << "WinIdChange" << hex << effectiveWinId();
            if(effectiveWinId() == 0)
                break;

            // Sometimes Qt needs to re-create the underlying window of the widget and
            // the winId() may be changed at runtime. So we need to reset all X11 properties
            // when this happens.
            qDebug() << "WinIdChange" << Qt::hex << effectiveWinId() << "handle" << windowHandle() << windowHandle()->screen();

            // Qt::WA_X11NetWmWindowTypeDock becomes ineffective in Qt 5
            // See QTBUG-39887: https://bugreports.qt-project.org/browse/QTBUG-39887
            // Let's use KWindowSystem for that
            KX11Extras::setType(effectiveWinId(), NET::Dock);

            updateWmStrut(); // reserve screen space for the panel
            KX11Extras::setOnAllDesktops(effectiveWinId(), true);
        }
        break;
    }
    case QEvent::DragEnter:
        dynamic_cast<QDropEvent *>(event)->setDropAction(Qt::IgnoreAction);
        event->accept();
#if __cplusplus >= 201703L
        [[fallthrough]];
#endif
        // fall through
    case QEvent::Enter:
        mShowDelayTimer.start();
        break;

    case QEvent::Leave:
    case QEvent::DragLeave:
        mShowDelayTimer.stop();
        hidePanel();
        break;

#if (QT_VERSION >= QT_VERSION_CHECK(6,8,0))
    case QEvent::Paint:
    // NOTE: Starting from Qt 6.8.0, random artifacts are possible in
    // translucent windows under Wayland. This a workaround.
    if (QGuiApplication::platformName() == QStringLiteral("wayland"))
    {
        QPainter p(this);
        p.setClipRegion(static_cast<QPaintEvent*>(event)->region());
        auto origMode = p.compositionMode();
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.fillRect(rect(), Qt::transparent);
        p.setCompositionMode(origMode);
    }
    break;
#endif

    default:
        break;
    }

    return QFrame::event(event);
}

/************************************************

 ************************************************/

void LXQtPanel::showEvent(QShowEvent *event)
{
    QFrame::showEvent(event);
    realign();
}


/************************************************

 ************************************************/
void LXQtPanel::showPopupMenu(const QPoint& cursorPos, Plugin *plugin)
{
    PopupMenu * menu = new PopupMenu(tr("Panel"), this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    menu->setIcon(XdgIcon::fromTheme(QStringLiteral("configure-toolbars")));

    // Plugin Menu ..............................
    if (plugin)
    {
        QMenu *m = plugin->popupMenu();

        if (m)
        {
            menu->addTitle(plugin->windowTitle());
            const auto actions = m->actions();
            for (auto const & action : actions)
            {
                action->setParent(menu);
                action->setDisabled(mLockPanel);
                menu->addAction(action);
            }
            delete m;
        }
    }

    // Panel menu ...............................

    menu->addTitle(QIcon(), tr("Panel"));

    menu->addAction(XdgIcon::fromTheme(QLatin1String("configure")),
                   tr("Configure Panel"),
                   this, &LXQtPanel::showConfigDialog
                  )->setDisabled(mLockPanel);

    menu->addAction(XdgIcon::fromTheme(QStringLiteral("preferences-plugin")),
                   tr("Manage Widgets"),
                   this, &LXQtPanel::showAddPluginDialog
                  )->setDisabled(mLockPanel);

    LXQtPanelApplication *a = reinterpret_cast<LXQtPanelApplication*>(qApp);
    menu->addAction(XdgIcon::fromTheme(QLatin1String("list-add")),
                   tr("Add New Panel"),
                   a, &LXQtPanelApplication::addNewPanel
                  );

    if (a->count() > 1)
    {
        menu->addAction(XdgIcon::fromTheme(QLatin1String("list-remove")),
                       tr("Remove Panel", "Menu Item"),
                       this, &LXQtPanel::userRequestForDeletion
                      )->setDisabled(mLockPanel);
    }

    QAction * act_lock = menu->addAction(tr("Lock This Panel"));
    act_lock->setCheckable(true);
    act_lock->setChecked(mLockPanel);
    connect(act_lock, &QAction::triggered, this, [this] { mLockPanel = !mLockPanel; saveSettings(false); });

#ifdef DEBUG
    menu->addSeparator();
    menu->addAction("Exit (debug only)", qApp, &QApplication::quit);
#endif

    /* Note: in multihead & multipanel setup the QMenu::popup/exec places the window
     * sometimes wrongly (it seems that this bug is somehow connected to misinterpretation
     * of QDesktopWidget::availableGeometry)
     */
    menu->setGeometry(calculatePopupWindowPos(cursorPos, menu->sizeHint()));
    willShowWindow(menu);
    menu->show();
}

Plugin* LXQtPanel::findPlugin(const ILXQtPanelPlugin* iPlugin) const
{
    const auto plugins = mPlugins->plugins();
    for (auto const & plug : plugins)
        if (plug->iPlugin() == iPlugin)
            return plug;
    return nullptr;
}

/************************************************

 ************************************************/
QRect LXQtPanel::calculatePopupWindowPos(QPoint const & absolutePos, QSize const & windowSize) const
{
    // Using of anchors makes coordinates be absolute under some Wayland compositors.
    // Therefore, to cover both X11 and Wayland, we first use the local coordinates
    // and then map them to the global coordinates.
    QPoint localPos = mapFromGlobal(absolutePos);
    int x = localPos.x(), y = localPos.y();

    switch (position())
    {
    case ILXQtPanel::PositionTop:
        y = mGeometry.height();
        break;

    case ILXQtPanel::PositionBottom:
        y = -windowSize.height();
        break;

    case ILXQtPanel::PositionLeft:
        x = mGeometry.width();
        break;

    case ILXQtPanel::PositionRight:
        x = -windowSize.width();
        break;
    }

    QRect res(mapToGlobal(QPoint(x, y)), windowSize);

    if (qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
        return res;

    QRect panelScreen;
    const auto screens = QApplication::screens();
    if (mActualScreenNum < screens.size())
        panelScreen = screens.at(mActualScreenNum)->geometry();
    // NOTE: We cannot use AvailableGeometry() which returns the work area here because when in a
    // multihead setup with different resolutions. In this case, the size of the work area is limited
    // by the smallest monitor and may be much smaller than the current screen and we will place the
    // menu at the wrong place. This is very bad for UX. So let's use the full size of the screen.
    if (res.right() > panelScreen.right())
        res.moveRight(panelScreen.right());

    if (res.bottom() > panelScreen.bottom())
        res.moveBottom(panelScreen.bottom());

    if (res.left() < panelScreen.left())
        res.moveLeft(panelScreen.left());

    if (res.top() < panelScreen.top())
        res.moveTop(panelScreen.top());

    return res;
}

/************************************************

 ************************************************/
QRect LXQtPanel::calculatePopupWindowPos(const ILXQtPanelPlugin *plugin, const QSize &windowSize) const
{
    Plugin *panel_plugin = findPlugin(plugin);
    if (nullptr == panel_plugin)
    {
        qWarning() << Q_FUNC_INFO << "Wrong logic? Unable to find Plugin* for" << plugin << "known plugins follow...";
        const auto plugins = mPlugins->plugins();
        for (auto const & plug : plugins)
            qWarning() << plug->iPlugin() << plug;

        return QRect();
    }

    // Note: assuming there are not contentMargins around the "BackgroundWidget" (LXQtPanelWidget)
    return calculatePopupWindowPos(mapToGlobal(panel_plugin->geometry().topLeft()), windowSize);
}


/************************************************

 ************************************************/
void LXQtPanel::willShowWindow(QWidget * w)
{
    mStandaloneWindows->observeWindow(w);
}

/************************************************

 ************************************************/
void LXQtPanel::pluginFlagsChanged(const ILXQtPanelPlugin * /*plugin*/)
{
    mLayout->rebuild();
}

/************************************************

 ************************************************/
QString LXQtPanel::qssPosition() const
{
    return positionToStr(position());
}

/************************************************

 ************************************************/
void LXQtPanel::pluginMoved(Plugin * plug)
{
    //get new position of the moved plugin
    bool found{false};
    QString plug_is_before;
    for (int i=0; i<mLayout->count(); ++i)
    {
        Plugin *plugin = qobject_cast<Plugin*>(mLayout->itemAt(i)->widget());
        if (plugin)
        {
            if (found)
            {
                //we found our plugin in previous cycle -> is before this (or empty as last)
                plug_is_before = plugin->settingsGroup();
                break;
            } else
                found = (plug == plugin);
        }
    }
    mPlugins->movePlugin(plug, plug_is_before);
}


/************************************************

 ************************************************/
void LXQtPanel::userRequestForDeletion()
{
    const QMessageBox::StandardButton ret
        = QMessageBox::warning(this, tr("Remove Panel", "Dialog Title") ,
            tr("Removing a panel can not be undone.\nDo you want to remove this panel?"),
            QMessageBox::Yes | QMessageBox::No);

    if (ret != QMessageBox::Yes) {
        return;
    }

    mSettings->beginGroup(mConfigGroup);
    const QStringList plugins = mSettings->value(QStringLiteral("plugins")).toStringList();
    mSettings->endGroup();

    for(const QString& i : plugins)
        if (!i.isEmpty())
            mSettings->remove(i);

    mSettings->remove(mConfigGroup);

    emit deletedByUser(this);
}

bool LXQtPanel::isPanelOverlapped() const
{
    LXQtPanelApplication *a = reinterpret_cast<LXQtPanelApplication*>(qApp);

    //TODO: calculate geometry on wayland
    QRect area = mGeometry;
    return a->getWMBackend()->isAreaOverlapped(area);
}

void LXQtPanel::showPanel(bool animate)
{
    if (mHidable)
    {
        mHideTimer.stop();
        if (mHidden)
        {
            mHidden = false;
            setPanelGeometry(mAnimationTime > 0 && animate);
        }
    }
}

void LXQtPanel::hidePanel()
{
    if (mHidable && !mHidden
        && !mStandaloneWindows->isAnyWindowShown())
    {
        mHideTimer.start();
    }
}

void LXQtPanel::hidePanelWork()
{
    if (!testAttribute(Qt::WA_UnderMouse))
    {
        if (!mStandaloneWindows->isAnyWindowShown())
        {
            if (!mHideOnOverlap || isPanelOverlapped())
            {
                mHidden = true;
                setPanelGeometry(mAnimationTime > 0);
            }
        }
        else
        {
            mHideTimer.start();
        }
    }
}

void LXQtPanel::setHidable(bool hidable, bool save)
{
    if (mHidable == hidable)
        return;

    mHidable = hidable;

    if (save)
        saveSettings(true);

    realign();
}

void LXQtPanel::setVisibleMargin(bool visibleMargin, bool save)
{
    if (mVisibleMargin == visibleMargin)
        return;

    mVisibleMargin = visibleMargin;

    if (save)
        saveSettings(true);

    realign();
}

void LXQtPanel::setHideOnOverlap(bool hideOnOverlap, bool save)
{
    if (mHideOnOverlap == hideOnOverlap)
        return;

    mHideOnOverlap = hideOnOverlap;

    if (save)
        saveSettings(true);

    realign();
}

void LXQtPanel::setAnimationTime(int animationTime, bool save)
{
    if (mAnimationTime == animationTime)
        return;

    mAnimationTime = animationTime;

    if (save)
        saveSettings(true);
}

void LXQtPanel::setShowDelay(int showDelay, bool save)
{
    if (mShowDelayTimer.interval() == showDelay)
        return;

    mShowDelayTimer.setInterval(showDelay);

    if (save)
        saveSettings(true);
}

QString LXQtPanel::iconTheme() const
{
    return mSettings->value(QStringLiteral("iconTheme")).toString();
}

void LXQtPanel::setIconTheme(const QString& iconTheme)
{
    LXQtPanelApplication *a = reinterpret_cast<LXQtPanelApplication*>(qApp);
    a->setIconTheme(iconTheme);
}

void LXQtPanel::updateConfigDialog() const
{
    if (!mConfigDialog.isNull() && mConfigDialog->isVisible())
    {
        mConfigDialog->updateIconThemeSettings();
        const QList<QWidget*> widgets = mConfigDialog->findChildren<QWidget*>();
        for (QWidget *widget : widgets)
            widget->update();
    }
}

bool LXQtPanel::isPluginSingletonAndRunning(QString const & pluginId) const
{
    Plugin const * plugin = mPlugins->pluginByID(pluginId);
    if (nullptr == plugin)
        return false;
    else
        return plugin->iPlugin()->flags().testFlag(ILXQtPanelPlugin::SingleInstance);
}
