/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the plugins of the Qt Toolkit.
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

#include "qwaylandinputdevice_p.h"

#include "qwaylandintegration_p.h"
#include "qwaylandwindow_p.h"
#include "qwaylandsurface_p.h"
#include "qwaylandbuffer_p.h"
#if QT_CONFIG(wayland_datadevice)
#include "qwaylanddatadevice_p.h"
#include "qwaylanddatadevicemanager_p.h"
#endif
#if QT_CONFIG(wayland_client_primary_selection)
#include "qwaylandprimaryselectionv1_p.h"
#endif
#if QT_CONFIG(tabletevent)
#include "qwaylandtabletv2_p.h"
#endif
#include "qwaylandtouch_p.h"
#include "qwaylandscreen_p.h"
#include "qwaylandcursor_p.h"
#include "qwaylanddisplay_p.h"
#include "qwaylandshmbackingstore_p.h"
#include "qwaylandinputcontext_p.h"

#include <QtGui/private/qpixmap_raster_p.h>
#include <QtGui/private/qguiapplication_p.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qplatforminputcontext.h>
#include <QDebug>

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#if QT_CONFIG(cursor)
#include <wayland-cursor.h>
#endif

#include <QtGui/QGuiApplication>

QT_BEGIN_NAMESPACE

namespace QtWaylandClient {

Q_LOGGING_CATEGORY(lcQpaWaylandInput, "qt.qpa.wayland.input");

QWaylandInputDevice::Keyboard::Keyboard(QWaylandInputDevice *p)
    : mParent(p)
{
    mRepeatTimer.callOnTimeout([&]() {
        if (!focusWindow()) {
            // We destroyed the keyboard focus surface, but the server didn't get the message yet...
            // or the server didn't send an enter event first.
            return;
        }
        mRepeatTimer.setInterval(1000 / mRepeatRate);
        Qt::KeyboardModifiers modifiers = this->modifiers();
        handleKey(mRepeatKey.time, QEvent::KeyRelease, mRepeatKey.key, modifiers,
                  mRepeatKey.code, mRepeatKey.nativeVirtualKey, this->mNativeModifiers,
                  mRepeatKey.text, true);
        handleKey(mRepeatKey.time, QEvent::KeyPress, mRepeatKey.key, modifiers,
                  mRepeatKey.code, mRepeatKey.nativeVirtualKey, this->mNativeModifiers,
                  mRepeatKey.text, true);
    });
}

#if QT_CONFIG(xkbcommon)
bool QWaylandInputDevice::Keyboard::createDefaultKeymap()
{
    struct xkb_context *ctx = mParent->mQDisplay->xkbContext();
    if (!ctx)
        return false;

    struct xkb_rule_names names;
    names.rules = "evdev";
    names.model = "pc105";
    names.layout = "us";
    names.variant = "";
    names.options = "";

    mXkbKeymap.reset(xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS));
    if (mXkbKeymap)
        mXkbState.reset(xkb_state_new(mXkbKeymap.get()));

    if (!mXkbKeymap || !mXkbState) {
        qCWarning(lcQpaWayland, "failed to create default keymap");
        return false;
    }

    return true;
}
#endif

QWaylandInputDevice::Keyboard::~Keyboard()
{
    if (mFocus)
        QWindowSystemInterface::handleWindowActivated(nullptr);
    if (mParent->mVersion >= 3)
        wl_keyboard_release(object());
    else
        wl_keyboard_destroy(object());
}

QWaylandWindow *QWaylandInputDevice::Keyboard::focusWindow() const
{
    return mFocus ? QWaylandWindow::fromWlSurface(mFocus) : nullptr;
}

QWaylandInputDevice::Pointer::Pointer(QWaylandInputDevice *seat)
    : mParent(seat)
{
#if QT_CONFIG(cursor)
    mCursor.frameTimer.setSingleShot(true);
    mCursor.frameTimer.callOnTimeout([&]() {
        cursorTimerCallback();
    });
#endif
}

QWaylandInputDevice::Pointer::~Pointer()
{
    if (mParent->mVersion >= 3)
        wl_pointer_release(object());
    else
        wl_pointer_destroy(object());
}

QWaylandWindow *QWaylandInputDevice::Pointer::focusWindow() const
{
    return mFocus ? mFocus->waylandWindow() : nullptr;
}

#if QT_CONFIG(cursor)

class WlCallback : public QtWayland::wl_callback {
public:
    explicit WlCallback(::wl_callback *callback, std::function<void(uint32_t)> fn, bool autoDelete = false)
        : QtWayland::wl_callback(callback)
        , m_fn(fn)
        , m_autoDelete(autoDelete)
    {}
    ~WlCallback() override { wl_callback_destroy(object()); }
    bool done() const { return m_done; }
    void callback_done(uint32_t callback_data) override {
        m_done = true;
        m_fn(callback_data);
        if (m_autoDelete)
            delete this;
    }
private:
    bool m_done = false;
    std::function<void(uint32_t)> m_fn;
    bool m_autoDelete = false;
};

class CursorSurface : public QWaylandSurface
{
public:
    explicit CursorSurface(QWaylandInputDevice::Pointer *pointer, QWaylandDisplay *display)
        : QWaylandSurface(display)
        , m_pointer(pointer)
    {
        //TODO: When we upgrade to libwayland 1.10, use wl_surface_get_version instead.
        m_version = display->compositorVersion();
        connect(this, &QWaylandSurface::screensChanged,
                m_pointer, &QWaylandInputDevice::Pointer::updateCursor);
    }

    void hide()
    {
        uint serial = m_pointer->mEnterSerial;
        Q_ASSERT(serial);
        m_pointer->set_cursor(serial, nullptr, 0, 0);
        m_setSerial = 0;
    }

    // Size and hotspot are in surface coordinates
    void update(wl_buffer *buffer, const QPoint &hotspot, const QSize &size, int bufferScale, bool animated = false)
    {
        // Calling code needs to ensure buffer scale is supported if != 1
        Q_ASSERT(bufferScale == 1 || m_version >= 3);

        auto enterSerial = m_pointer->mEnterSerial;
        if (m_setSerial < enterSerial || m_hotspot != hotspot) {
            m_pointer->set_cursor(m_pointer->mEnterSerial, object(), hotspot.x(), hotspot.y());
            m_setSerial = enterSerial;
            m_hotspot = hotspot;
        }

        if (m_version >= 3)
            set_buffer_scale(bufferScale);

        attach(buffer, 0, 0);
        damage(0, 0, size.width(), size.height());
        m_frameCallback.reset();
        if (animated) {
            m_frameCallback.reset(new WlCallback(frame(), [this](uint32_t time){
               Q_UNUSED(time);
               m_pointer->cursorFrameCallback();
            }));
        }
        commit();
    }

    int outputScale() const
    {
        int scale = 0;
        for (auto *screen : m_screens)
            scale = qMax(scale, screen->scale());
        return scale;
    }

private:
    QScopedPointer<WlCallback> m_frameCallback;
    QWaylandInputDevice::Pointer *m_pointer = nullptr;
    uint m_version = 0;
    uint m_setSerial = 0;
    QPoint m_hotspot;
};

QString QWaylandInputDevice::Pointer::cursorThemeName() const
{
    static QString themeName = qEnvironmentVariable("XCURSOR_THEME", QStringLiteral("default"));
    return themeName;
}

int QWaylandInputDevice::Pointer::cursorSize() const
{
    constexpr int defaultCursorSize = 24;
    static const int xCursorSize = qEnvironmentVariableIntValue("XCURSOR_SIZE");
    return xCursorSize > 0 ? xCursorSize : defaultCursorSize;
}

int QWaylandInputDevice::Pointer::idealCursorScale() const
{
    // set_buffer_scale is not supported on earlier versions
    if (seat()->mQDisplay->compositorVersion() < 3)
        return 1;

    if (auto *s = mCursor.surface.data()) {
        if (s->outputScale() > 0)
            return s->outputScale();
    }

    return seat()->mCursor.fallbackOutputScale;
}

void QWaylandInputDevice::Pointer::updateCursorTheme()
{
    int scale = idealCursorScale();
    int pixelSize = cursorSize() * scale;
    auto *display = seat()->mQDisplay;
    mCursor.theme = display->loadCursorTheme(cursorThemeName(), pixelSize);

    if (!mCursor.theme)
        return; // A warning has already been printed in loadCursorTheme

    if (auto *arrow = mCursor.theme->cursor(Qt::ArrowCursor)) {
        int arrowPixelSize = qMax(arrow->images[0]->width, arrow->images[0]->height); // Not all cursor themes are square
        while (scale > 1 && arrowPixelSize / scale < cursorSize())
            --scale;
    } else {
        qCWarning(lcQpaWayland) << "Cursor theme does not support the arrow cursor";
    }
    mCursor.themeBufferScale = scale;
}

void QWaylandInputDevice::Pointer::updateCursor()
{
    if (mEnterSerial == 0)
        return;

    auto shape = seat()->mCursor.shape;

    if (shape == Qt::BlankCursor) {
        if (mCursor.surface)
            mCursor.surface->hide();
        return;
    }

    if (shape == Qt::BitmapCursor) {
        auto buffer = seat()->mCursor.bitmapBuffer;
        if (!buffer) {
            qCWarning(lcQpaWayland) << "No buffer for bitmap cursor, can't set cursor";
            return;
        }
        auto hotspot = seat()->mCursor.hotspot;
        int bufferScale = seat()->mCursor.bitmapScale;
        getOrCreateCursorSurface()->update(buffer->buffer(), hotspot, buffer->size(), bufferScale);
        return;
    }

    if (!mCursor.theme || idealCursorScale() != mCursor.themeBufferScale)
        updateCursorTheme();

    if (!mCursor.theme)
        return;

    // Set from shape using theme
    uint time = seat()->mCursor.animationTimer.elapsed();

    if (struct ::wl_cursor *waylandCursor = mCursor.theme->cursor(shape)) {
        uint duration = 0;
        int frame = wl_cursor_frame_and_duration(waylandCursor, time, &duration);
        ::wl_cursor_image *image = waylandCursor->images[frame];

        struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
        if (!buffer) {
            qCWarning(lcQpaWayland) << "Could not find buffer for cursor" << shape;
            return;
        }
        int bufferScale = mCursor.themeBufferScale;
        QPoint hotspot = QPoint(image->hotspot_x, image->hotspot_y) / bufferScale;
        QSize size = QSize(image->width, image->height) / bufferScale;
        bool animated = duration > 0;
        if (animated) {
            mCursor.gotFrameCallback = false;
            mCursor.gotTimerCallback = false;
            mCursor.frameTimer.start(duration);
        }
        getOrCreateCursorSurface()->update(buffer, hotspot, size, bufferScale, animated);
        return;
    }

    qCWarning(lcQpaWayland) << "Unable to change to cursor" << shape;
}

CursorSurface *QWaylandInputDevice::Pointer::getOrCreateCursorSurface()
{
    if (!mCursor.surface)
        mCursor.surface.reset(new CursorSurface(this, seat()->mQDisplay));
    return mCursor.surface.get();
}

void QWaylandInputDevice::Pointer::cursorTimerCallback()
{
    mCursor.gotTimerCallback = true;
    if (mCursor.gotFrameCallback) {
        updateCursor();
    }
}

void QWaylandInputDevice::Pointer::cursorFrameCallback()
{
    mCursor.gotFrameCallback = true;
    if (mCursor.gotTimerCallback) {
        updateCursor();
    }
}

#endif // QT_CONFIG(cursor)

QWaylandInputDevice::Touch::Touch(QWaylandInputDevice *p)
    : mParent(p)
{
}

QWaylandInputDevice::Touch::~Touch()
{
    if (mParent->mVersion >= 3)
        wl_touch_release(object());
    else
        wl_touch_destroy(object());
}

QWaylandInputDevice::QWaylandInputDevice(QWaylandDisplay *display, int version, uint32_t id)
    : QtWayland::wl_seat(display->wl_registry(), id, qMin(version, 5))
    , mQDisplay(display)
    , mDisplay(display->wl_display())
    , mVersion(qMin(version, 5))
{
#if QT_CONFIG(wayland_datadevice)
    if (mQDisplay->dndSelectionHandler()) {
        mDataDevice = mQDisplay->dndSelectionHandler()->getDataDevice(this);
    }
#endif

#if QT_CONFIG(wayland_client_primary_selection)
    // TODO: Could probably decouple this more if there was a signal for new seat added
    if (auto *psm = mQDisplay->primarySelectionManager())
        setPrimarySelectionDevice(psm->createDevice(this));
#endif

    if (mQDisplay->textInputManager())
        mTextInput.reset(new QWaylandTextInput(mQDisplay, mQDisplay->textInputManager()->get_text_input(wl_seat())));

#if QT_CONFIG(tabletevent)
    if (auto *tm = mQDisplay->tabletManager())
        mTabletSeat.reset(new QWaylandTabletSeatV2(tm, this));
#endif
}

QWaylandInputDevice::~QWaylandInputDevice()
{
    delete mPointer;
    delete mKeyboard;
    delete mTouch;
}

void QWaylandInputDevice::seat_capabilities(uint32_t caps)
{
    mCaps = caps;

    if (caps & WL_SEAT_CAPABILITY_KEYBOARD && !mKeyboard) {
        mKeyboard = createKeyboard(this);
        mKeyboard->init(get_keyboard());
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && mKeyboard) {
        delete mKeyboard;
        mKeyboard = nullptr;
    }

    if (caps & WL_SEAT_CAPABILITY_POINTER && !mPointer) {
        mPointer = createPointer(this);
        mPointer->init(get_pointer());
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && mPointer) {
        delete mPointer;
        mPointer = nullptr;
    }

    if (caps & WL_SEAT_CAPABILITY_TOUCH && !mTouch) {
        mTouch = createTouch(this);
        mTouch->init(get_touch());

        if (!mTouchDevice) {
            mTouchDevice = new QTouchDevice;
            mTouchDevice->setType(QTouchDevice::TouchScreen);
            mTouchDevice->setCapabilities(QTouchDevice::Position);
            QWindowSystemInterface::registerTouchDevice(mTouchDevice);
        }
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && mTouch) {
        delete mTouch;
        mTouch = nullptr;
    }
}

QWaylandInputDevice::Keyboard *QWaylandInputDevice::createKeyboard(QWaylandInputDevice *device)
{
    return new Keyboard(device);
}

QWaylandInputDevice::Pointer *QWaylandInputDevice::createPointer(QWaylandInputDevice *device)
{
    return new Pointer(device);
}

QWaylandInputDevice::Touch *QWaylandInputDevice::createTouch(QWaylandInputDevice *device)
{
    return new Touch(device);
}

QWaylandInputDevice::Keyboard *QWaylandInputDevice::keyboard() const
{
    return mKeyboard;
}

QWaylandInputDevice::Pointer *QWaylandInputDevice::pointer() const
{
    return mPointer;
}

QWaylandInputDevice::Touch *QWaylandInputDevice::touch() const
{
    return mTouch;
}

void QWaylandInputDevice::handleEndDrag()
{
    if (mTouch)
        mTouch->releasePoints();
    if (mPointer)
        mPointer->releaseButtons();
}

#if QT_CONFIG(wayland_datadevice)
void QWaylandInputDevice::setDataDevice(QWaylandDataDevice *device)
{
    mDataDevice = device;
}

QWaylandDataDevice *QWaylandInputDevice::dataDevice() const
{
    return mDataDevice;
}
#endif

#if QT_CONFIG(wayland_client_primary_selection)
void QWaylandInputDevice::setPrimarySelectionDevice(QWaylandPrimarySelectionDeviceV1 *primarySelectionDevice)
{
    mPrimarySelectionDevice.reset(primarySelectionDevice);
}

QWaylandPrimarySelectionDeviceV1 *QWaylandInputDevice::primarySelectionDevice() const
{
    return mPrimarySelectionDevice.data();
}
#endif

void QWaylandInputDevice::setTextInput(QWaylandTextInput *textInput)
{
    mTextInput.reset(textInput);
}

QWaylandTextInput *QWaylandInputDevice::textInput() const
{
    return mTextInput.data();
}

void QWaylandInputDevice::removeMouseButtonFromState(Qt::MouseButton button)
{
    if (mPointer)
        mPointer->mButtons = mPointer->mButtons & !button;
}

QWaylandWindow *QWaylandInputDevice::pointerFocus() const
{
    return mPointer ? mPointer->focusWindow() : nullptr;
}

QWaylandWindow *QWaylandInputDevice::keyboardFocus() const
{
    return mKeyboard ? mKeyboard->focusWindow() : nullptr;
}

QWaylandWindow *QWaylandInputDevice::touchFocus() const
{
    return mTouch ? mTouch->mFocus : nullptr;
}

QPointF QWaylandInputDevice::pointerSurfacePosition() const
{
    return mPointer ? mPointer->mSurfacePos : QPointF();
}

QList<int> QWaylandInputDevice::possibleKeys(const QKeyEvent *event) const
{
#if QT_CONFIG(xkbcommon)
    if (mKeyboard && mKeyboard->mXkbState)
        return QXkbCommon::possibleKeys(mKeyboard->mXkbState.get(), event);
#else
    Q_UNUSED(event);
#endif
    return {};
}

Qt::KeyboardModifiers QWaylandInputDevice::modifiers() const
{
    if (!mKeyboard)
        return Qt::NoModifier;

    return mKeyboard->modifiers();
}

Qt::KeyboardModifiers QWaylandInputDevice::Keyboard::modifiers() const
{
    Qt::KeyboardModifiers ret = Qt::NoModifier;

#if QT_CONFIG(xkbcommon)
    if (!mXkbState)
        return ret;

    ret = QXkbCommon::modifiers(mXkbState.get());
#endif

    return ret;
}

#if QT_CONFIG(cursor)
void QWaylandInputDevice::setCursor(const QCursor *cursor, const QSharedPointer<QWaylandBuffer> &cachedBuffer, int fallbackOutputScale)
{
    CursorState oldCursor = mCursor;
    mCursor = CursorState(); // Clear any previous state
    mCursor.shape = cursor ? cursor->shape() : Qt::ArrowCursor;
    mCursor.hotspot = cursor ? cursor->hotSpot() : QPoint();
    mCursor.fallbackOutputScale = fallbackOutputScale;
    mCursor.animationTimer.start();

    if (mCursor.shape == Qt::BitmapCursor) {
        mCursor.bitmapBuffer = cachedBuffer ? cachedBuffer : QWaylandCursor::cursorBitmapBuffer(mQDisplay, cursor);
        qreal dpr = cursor->pixmap().devicePixelRatio();
        mCursor.bitmapScale = int(dpr); // Wayland doesn't support fractional buffer scale
        // If there was a fractional part of the dpr, we need to scale the hotspot accordingly
        if (mCursor.bitmapScale < dpr)
            mCursor.hotspot *= dpr / mCursor.bitmapScale;
    }

    // Return early if setCursor was called redundantly (mostly happens from decorations)
    if (mCursor.shape != Qt::BitmapCursor
            && mCursor.shape == oldCursor.shape
            && mCursor.hotspot == oldCursor.hotspot
            && mCursor.fallbackOutputScale == oldCursor.fallbackOutputScale) {
        return;
    }

    if (mPointer)
        mPointer->updateCursor();
}
#endif

class EnterEvent : public QWaylandPointerEvent
{
public:
    EnterEvent(QWaylandWindow *surface, const QPointF &local, const QPointF &global)
        : QWaylandPointerEvent(QEvent::Enter, Qt::NoScrollPhase, surface, 0,
                               local, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier)
    {}
};

void QWaylandInputDevice::Pointer::pointer_enter(uint32_t serial, struct wl_surface *surface,
                                                 wl_fixed_t sx, wl_fixed_t sy)
{
    if (!surface)
        return;

    QWaylandWindow *window = QWaylandWindow::fromWlSurface(surface);

    if (!window)
        return; // Ignore foreign surfaces

    if (mFocus) {
        qCWarning(lcQpaWayland) << "The compositor sent a wl_pointer.enter event before sending a"
                                << "leave event first, this is not allowed by the wayland protocol"
                                << "attempting to work around it by invalidating the current focus";
        invalidateFocus();
    }
    mFocus = window->waylandSurface();
    connect(mFocus.data(), &QObject::destroyed, this, &Pointer::handleFocusDestroyed);

    mSurfacePos = QPointF(wl_fixed_to_double(sx), wl_fixed_to_double(sy));
    mGlobalPos = window->window()->mapToGlobal(mSurfacePos.toPoint());

    mParent->mSerial = serial;
    mEnterSerial = serial;

#if QT_CONFIG(cursor)
    // Depends on mEnterSerial being updated
    updateCursor();
#endif

    QWaylandWindow *grab = QWaylandWindow::mouseGrab();
    if (!grab)
        setFrameEvent(new EnterEvent(window, mSurfacePos, mGlobalPos));
}

class LeaveEvent : public QWaylandPointerEvent
{
public:
    LeaveEvent(QWaylandWindow *surface, const QPointF &localPos, const QPointF &globalPos)
        : QWaylandPointerEvent(QEvent::Leave, Qt::NoScrollPhase, surface, 0,
                               localPos, globalPos, Qt::NoButton, Qt::NoButton, Qt::NoModifier)
    {}
};

void QWaylandInputDevice::Pointer::pointer_leave(uint32_t time, struct wl_surface *surface)
{
    invalidateFocus();
    mButtons = Qt::NoButton;

    mParent->mTime = time;

    // The event may arrive after destroying the window, indicated by
    // a null surface.
    if (!surface)
        return;

    auto *window = QWaylandWindow::fromWlSurface(surface);
    if (!window)
        return; // Ignore foreign surfaces

    if (!QWaylandWindow::mouseGrab())
        setFrameEvent(new LeaveEvent(window, mSurfacePos, mGlobalPos));
}

class MotionEvent : public QWaylandPointerEvent
{
public:
    MotionEvent(QWaylandWindow *surface, ulong timestamp, const QPointF &localPos,
                const QPointF &globalPos, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers)
        : QWaylandPointerEvent(QEvent::MouseMove, Qt::NoScrollPhase, surface,
                               timestamp, localPos, globalPos, buttons, Qt::NoButton, modifiers)
    {
    }
};

void QWaylandInputDevice::Pointer::pointer_motion(uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    QWaylandWindow *window = focusWindow();
    if (!window) {
        // We destroyed the pointer focus surface, but the server didn't get the message yet...
        // or the server didn't send an enter event first. In either case, ignore the event.
        return;
    }

    QPointF pos(wl_fixed_to_double(surface_x), wl_fixed_to_double(surface_y));
    QPointF delta = pos - pos.toPoint();
    QPointF global = window->window()->mapToGlobal(pos.toPoint());
    global += delta;

    mSurfacePos = pos;
    mGlobalPos = global;
    mParent->mTime = time;

    QWaylandWindow *grab = QWaylandWindow::mouseGrab();
    if (grab && grab != window) {
        // We can't know the true position since we're getting events for another surface,
        // so we just set it outside of the window boundaries.
        pos = QPointF(-1, -1);
        global = grab->window()->mapToGlobal(pos.toPoint());
        window = grab;
    }
    setFrameEvent(new MotionEvent(window, time, pos, global, mButtons, mParent->modifiers()));
}

class PressEvent : public QWaylandPointerEvent
{
public:
    PressEvent(QWaylandWindow *surface, ulong timestamp, const QPointF &localPos,
               const QPointF &globalPos, Qt::MouseButtons buttons, Qt::MouseButton button,
               Qt::KeyboardModifiers modifiers)
        : QWaylandPointerEvent(QEvent::MouseButtonPress, Qt::NoScrollPhase, surface,
                               timestamp, localPos, globalPos, buttons, button, modifiers)
    {
    }
};

class ReleaseEvent : public QWaylandPointerEvent
{
public:
    ReleaseEvent(QWaylandWindow *surface, ulong timestamp, const QPointF &localPos,
                 const QPointF &globalPos, Qt::MouseButtons buttons, Qt::MouseButton button,
                 Qt::KeyboardModifiers modifiers)
        : QWaylandPointerEvent(QEvent::MouseButtonRelease, Qt::NoScrollPhase, surface,
                               timestamp, localPos, globalPos, buttons, button, modifiers)
    {
    }
};

void QWaylandInputDevice::Pointer::pointer_button(uint32_t serial, uint32_t time,
                                                  uint32_t button, uint32_t state)
{
    QWaylandWindow *window = focusWindow();
    if (!window) {
        // We destroyed the pointer focus surface, but the server didn't get the message yet...
        // or the server didn't send an enter event first. In either case, ignore the event.
        return;
    }

    Qt::MouseButton qt_button;

    // translate from kernel (input.h) 'button' to corresponding Qt:MouseButton.
    // The range of mouse values is 0x110 <= mouse_button < 0x120, the first Joystick button.
    switch (button) {
    case 0x110: qt_button = Qt::LeftButton; break;    // kernel BTN_LEFT
    case 0x111: qt_button = Qt::RightButton; break;
    case 0x112: qt_button = Qt::MiddleButton; break;
    case 0x113: qt_button = Qt::ExtraButton1; break;  // AKA Qt::BackButton
    case 0x114: qt_button = Qt::ExtraButton2; break;  // AKA Qt::ForwardButton
    case 0x115: qt_button = Qt::ExtraButton3; break;  // AKA Qt::TaskButton
    case 0x116: qt_button = Qt::ExtraButton4; break;
    case 0x117: qt_button = Qt::ExtraButton5; break;
    case 0x118: qt_button = Qt::ExtraButton6; break;
    case 0x119: qt_button = Qt::ExtraButton7; break;
    case 0x11a: qt_button = Qt::ExtraButton8; break;
    case 0x11b: qt_button = Qt::ExtraButton9; break;
    case 0x11c: qt_button = Qt::ExtraButton10; break;
    case 0x11d: qt_button = Qt::ExtraButton11; break;
    case 0x11e: qt_button = Qt::ExtraButton12; break;
    case 0x11f: qt_button = Qt::ExtraButton13; break;
    default: return; // invalid button number (as far as Qt is concerned)
    }

    if (state)
        mButtons |= qt_button;
    else
        mButtons &= ~qt_button;

    mParent->mTime = time;
    mParent->mSerial = serial;
    if (state)
        mParent->mQDisplay->setLastInputDevice(mParent, serial, window);

    QWaylandWindow *grab = QWaylandWindow::mouseGrab();

    QPointF pos = mSurfacePos;
    QPointF global = mGlobalPos;
    if (grab && grab != focusWindow()) {
        pos = QPointF(-1, -1);
        global = grab->window()->mapToGlobal(pos.toPoint());

        window = grab;
    }

    if (state)
        setFrameEvent(new PressEvent(window, time, pos, global, mButtons, qt_button, mParent->modifiers()));
    else
        setFrameEvent(new ReleaseEvent(window, time, pos, global, mButtons, qt_button, mParent->modifiers()));
}

void QWaylandInputDevice::Pointer::invalidateFocus()
{
    if (mFocus) {
        disconnect(mFocus.data(), &QObject::destroyed, this, &Pointer::handleFocusDestroyed);
        mFocus = nullptr;
    }
    mEnterSerial = 0;
}

void QWaylandInputDevice::Pointer::releaseButtons()
{
    mButtons = Qt::NoButton;

    if (auto *window = focusWindow()) {
        ReleaseEvent e(focusWindow(), mParent->mTime, mSurfacePos, mGlobalPos, mButtons, Qt::NoButton, mParent->modifiers());
        window->handleMouse(mParent, e);
    }
}

class WheelEvent : public QWaylandPointerEvent
{
public:
    WheelEvent(QWaylandWindow *surface, Qt::ScrollPhase phase, ulong timestamp, const QPointF &local,
               const QPointF &global, const QPoint &pixelDelta, const QPoint &angleDelta,
               Qt::MouseEventSource source, Qt::KeyboardModifiers modifiers)
        : QWaylandPointerEvent(QEvent::Wheel, phase, surface, timestamp,
                               local, global, pixelDelta, angleDelta, source, modifiers)
    {
    }
};

void QWaylandInputDevice::Pointer::pointer_axis(uint32_t time, uint32_t axis, int32_t value)
{
    if (!focusWindow()) {
        // We destroyed the pointer focus surface, but the server didn't get the message yet...
        // or the server didn't send an enter event first. In either case, ignore the event.
        return;
    }

    // Get the delta and convert it into the expected range
    switch (axis) {
    case WL_POINTER_AXIS_VERTICAL_SCROLL:
        mFrameData.delta.ry() += wl_fixed_to_double(value);
        qCDebug(lcQpaWaylandInput) << "wl_pointer.axis vertical:" << mFrameData.delta.y();
        break;
    case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
        mFrameData.delta.rx() += wl_fixed_to_double(value);
        qCDebug(lcQpaWaylandInput) << "wl_pointer.axis horizontal:" << mFrameData.delta.x();
        break;
    default:
        //TODO: is this really needed?
        qCWarning(lcQpaWaylandInput) << "wl_pointer.axis: Unknown axis:" << axis;
        return;
    }

    mParent->mTime = time;

    if (mParent->mVersion < WL_POINTER_FRAME_SINCE_VERSION) {
        qCDebug(lcQpaWaylandInput) << "Flushing new event; no frame event in this version";
        flushFrameEvent();
    }
}

void QWaylandInputDevice::Pointer::pointer_frame()
{
    flushFrameEvent();
}

void QWaylandInputDevice::Pointer::pointer_axis_source(uint32_t source)
{
    switch (source) {
    case axis_source_wheel:
        qCDebug(lcQpaWaylandInput) << "Axis source wheel";
        break;
    case axis_source_finger:
        qCDebug(lcQpaWaylandInput) << "Axis source finger";
        break;
    case axis_source_continuous:
        qCDebug(lcQpaWaylandInput) << "Axis source continuous";
        break;
    }
    mFrameData.axisSource = axis_source(source);
}

void QWaylandInputDevice::Pointer::pointer_axis_stop(uint32_t time, uint32_t axis)
{
    if (!focusWindow())
        return;

    mParent->mTime = time;
    switch (axis) {
    case axis_vertical_scroll:
        qCDebug(lcQpaWaylandInput) << "Received vertical wl_pointer.axis_stop";
        mFrameData.delta.setY(0); //TODO: what's the point of doing this?
        break;
    case axis_horizontal_scroll:
        qCDebug(lcQpaWaylandInput) << "Received horizontal wl_pointer.axis_stop";
        mFrameData.delta.setX(0);
        break;
    default:
        qCWarning(lcQpaWaylandInput) << "wl_pointer.axis_stop: Unknown axis: " << axis
                                     << "This is most likely a compositor bug";
        return;
    }

    // May receive axis_stop for events we haven't sent a ScrollBegin for because
    // most axis_sources do not mandate an axis_stop event to be sent.
    if (!mScrollBeginSent) {
        // TODO: For now, we just ignore these events, but we could perhaps take this as an
        // indication that this compositor will in fact send axis_stop events for these sources
        // and send a ScrollBegin the next time an axis_source event with this type is encountered.
        return;
    }

    QWaylandWindow *target = QWaylandWindow::mouseGrab();
    if (!target)
        target = focusWindow();
    Qt::KeyboardModifiers mods = mParent->modifiers();
    WheelEvent wheelEvent(focusWindow(), Qt::ScrollEnd, mParent->mTime, mSurfacePos, mGlobalPos,
                          QPoint(), QPoint(), Qt::MouseEventNotSynthesized, mods);
    target->handleMouse(mParent, wheelEvent);
    mScrollBeginSent = false;
    mScrollDeltaRemainder = QPointF();
}

void QWaylandInputDevice::Pointer::pointer_axis_discrete(uint32_t axis, int32_t value)
{
    if (!focusWindow())
        return;

    switch (axis) {
    case axis_vertical_scroll:
        qCDebug(lcQpaWaylandInput) << "wl_pointer.axis_discrete vertical:" << value;
        mFrameData.discreteDelta.ry() += value;
        break;
    case axis_horizontal_scroll:
        qCDebug(lcQpaWaylandInput) << "wl_pointer.axis_discrete horizontal:" << value;
        mFrameData.discreteDelta.rx() += value;
        break;
    default:
        //TODO: is this really needed?
        qCWarning(lcQpaWaylandInput) << "wl_pointer.axis_discrete: Unknown axis:" << axis;
        return;
    }
}

void QWaylandInputDevice::Pointer::setFrameEvent(QWaylandPointerEvent *event)
{
    qCDebug(lcQpaWaylandInput) << "Setting frame event " << event->type;
    if (mFrameData.event && mFrameData.event->type != event->type) {
        qCDebug(lcQpaWaylandInput) << "Flushing; previous was " << mFrameData.event->type;
        flushFrameEvent();
    }

    mFrameData.event = event;

    if (mParent->mVersion < WL_POINTER_FRAME_SINCE_VERSION) {
        qCDebug(lcQpaWaylandInput) << "Flushing new event; no frame event in this version";
        flushFrameEvent();
    }
}

void QWaylandInputDevice::Pointer::FrameData::resetScrollData()
{
    discreteDelta = QPoint();
    delta = QPointF();
    axisSource = axis_source_wheel;
}

bool QWaylandInputDevice::Pointer::FrameData::hasPixelDelta() const
{
    switch (axisSource) {
    case axis_source_wheel_tilt: // sideways tilt of the wheel
    case axis_source_wheel:
        // In the case of wheel events, a pixel delta doesn't really make sense,
        // and will make Qt think this is a continuous scroll event when it isn't,
        // so just ignore it.
        return false;
    case axis_source_finger:
    case axis_source_continuous:
        return !delta.isNull();
    default:
        return false;
    }
}

QPoint QWaylandInputDevice::Pointer::FrameData::pixelDeltaAndError(QPointF *accumulatedError) const
{
    if (!hasPixelDelta())
        return QPoint();

    Q_ASSERT(accumulatedError);
    // Add accumulated rounding error before rounding again
    QPoint pixelDelta = (delta + *accumulatedError).toPoint();
    *accumulatedError += delta - pixelDelta;
    Q_ASSERT(qAbs(accumulatedError->x()) < 1.0);
    Q_ASSERT(qAbs(accumulatedError->y()) < 1.0);

    // for continuous scroll events things should be
    // in the same direction
    // i.e converted so downwards surface co-ordinates (positive axis_value)
    // goes to downwards in wheel event (negative value)
    pixelDelta *= -1;
    return pixelDelta;
}

QPoint QWaylandInputDevice::Pointer::FrameData::angleDelta() const
{
    if (discreteDelta.isNull()) {
        // If we didn't get any discrete events, then we need to fall back to
        // the continuous information.
        return (delta * -12).toPoint(); //TODO: why multiply by 12?
    }

    // The angle delta is in eights of degrees, and our docs says most mice have
    // 1 click = 15 degrees. It's also in the opposite direction of surface space.
    return -discreteDelta * 15 * 8;
}

Qt::MouseEventSource QWaylandInputDevice::Pointer::FrameData::wheelEventSource() const
{
    switch (axisSource) {
    case axis_source_wheel_tilt: // sideways tilt of the wheel
    case axis_source_wheel:
        return Qt::MouseEventNotSynthesized;
    case axis_source_finger:
    case axis_source_continuous:
    default: // Whatever other sources might be added are probably not mouse wheels
        return Qt::MouseEventSynthesizedBySystem;
    }
}

void QWaylandInputDevice::Pointer::flushScrollEvent()
{
    QPoint angleDelta = mFrameData.angleDelta();

    // Angle delta is required for Qt wheel events, so don't try to send events if it's zero
    if (!angleDelta.isNull()) {
        QWaylandWindow *target = QWaylandWindow::mouseGrab();
        if (!target)
            target = focusWindow();

        if (isDefinitelyTerminated(mFrameData.axisSource) && !mScrollBeginSent) {
            qCDebug(lcQpaWaylandInput) << "Flushing scroll event sending ScrollBegin";
            target->handleMouse(mParent, WheelEvent(focusWindow(), Qt::ScrollBegin, mParent->mTime,
                                                    mSurfacePos, mGlobalPos, QPoint(), QPoint(),
                                                    Qt::MouseEventNotSynthesized,
                                                    mParent->modifiers()));
            mScrollBeginSent = true;
            mScrollDeltaRemainder = QPointF();
        }

        Qt::ScrollPhase phase = mScrollBeginSent ? Qt::ScrollUpdate : Qt::NoScrollPhase;
        QPoint pixelDelta = mFrameData.pixelDeltaAndError(&mScrollDeltaRemainder);
        Qt::MouseEventSource source = mFrameData.wheelEventSource();

        qCDebug(lcQpaWaylandInput) << "Flushing scroll event" << phase << pixelDelta << angleDelta;
        target->handleMouse(mParent, WheelEvent(focusWindow(), phase, mParent->mTime, mSurfacePos, mGlobalPos,
                                                pixelDelta, angleDelta, source, mParent->modifiers()));
    }

    mFrameData.resetScrollData();
}

void QWaylandInputDevice::Pointer::flushFrameEvent()
{
    if (auto *event = mFrameData.event) {
        if (auto window = event->surface) {
            window->handleMouse(mParent, *event);
        } else if (mFrameData.event->type == QEvent::MouseButtonRelease) {
            // If the window has been destroyed, we still need to report an up event, but it can't
            // be handled by the destroyed window (obviously), so send the event here instead.
            QWindowSystemInterface::handleMouseEvent(nullptr, event->timestamp, event->local,
                                 event->global, event->buttons,
                                 event->button, event->type,
                                 event->modifiers);// , Qt::MouseEventSource source = Qt::MouseEventNotSynthesized);
        }
        delete mFrameData.event;
        mFrameData.event = nullptr;
    }

    //TODO: do modifiers get passed correctly here?
    flushScrollEvent();
}

bool QWaylandInputDevice::Pointer::isDefinitelyTerminated(QtWayland::wl_pointer::axis_source source) const
{
    return source == axis_source_finger;
}

void QWaylandInputDevice::Keyboard::keyboard_keymap(uint32_t format, int32_t fd, uint32_t size)
{
    mKeymapFormat = format;
#if QT_CONFIG(xkbcommon)
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        qCWarning(lcQpaWayland) << "unknown keymap format:" << format;
        close(fd);
        return;
    }

    char *map_str = static_cast<char *>(mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
    if (map_str == MAP_FAILED) {
        close(fd);
        return;
    }

    mXkbKeymap.reset(xkb_keymap_new_from_string(mParent->mQDisplay->xkbContext(), map_str,
                                                XKB_KEYMAP_FORMAT_TEXT_V1,
                                                XKB_KEYMAP_COMPILE_NO_FLAGS));
    QXkbCommon::verifyHasLatinLayout(mXkbKeymap.get());

    munmap(map_str, size);
    close(fd);

    if (mXkbKeymap)
        mXkbState.reset(xkb_state_new(mXkbKeymap.get()));
    else
        mXkbState.reset(nullptr);
#else
    Q_UNUSED(fd);
    Q_UNUSED(size);
#endif
}

void QWaylandInputDevice::Keyboard::keyboard_enter(uint32_t time, struct wl_surface *surface, struct wl_array *keys)
{
    Q_UNUSED(time);
    Q_UNUSED(keys);

    if (!surface) {
        // Ignoring wl_keyboard.enter event with null surface. This is either a compositor bug,
        // or it's a race with a wl_surface.destroy request. In either case, ignore the event.
        return;
    }

    if (mFocus) {
        qCWarning(lcQpaWayland()) << "Unexpected wl_keyboard.enter event. Keyboard already has focus";
        disconnect(focusWindow(), &QWaylandWindow::wlSurfaceDestroyed, this, &Keyboard::handleFocusDestroyed);
    }

    mFocus = surface;
    connect(focusWindow(), &QWaylandWindow::wlSurfaceDestroyed, this, &Keyboard::handleFocusDestroyed);

    mParent->mQDisplay->handleKeyboardFocusChanged(mParent);
}

void QWaylandInputDevice::Keyboard::keyboard_leave(uint32_t time, struct wl_surface *surface)
{
    Q_UNUSED(time);

    if (!surface) {
        // Either a compositor bug, or a race condition with wl_surface.destroy, ignore the event.
        return;
    }

    if (surface != mFocus) {
        qCWarning(lcQpaWayland) << "Ignoring unexpected wl_keyboard.leave event."
                                << "wl_surface argument does not match the current focus"
                                << "This is most likely a compositor bug";
        return;
    }
    disconnect(focusWindow(), &QWaylandWindow::wlSurfaceDestroyed, this, &Keyboard::handleFocusDestroyed);
    handleFocusLost();
}

void QWaylandInputDevice::Keyboard::handleKey(ulong timestamp, QEvent::Type type, int key,
                                              Qt::KeyboardModifiers modifiers, quint32 nativeScanCode,
                                              quint32 nativeVirtualKey, quint32 nativeModifiers,
                                              const QString &text, bool autorepeat, ushort count)
{
    QPlatformInputContext *inputContext = QGuiApplicationPrivate::platformIntegration()->inputContext();
    bool filtered = false;

    if (inputContext) {
        QKeyEvent event(type, key, modifiers, nativeScanCode, nativeVirtualKey,
                        nativeModifiers, text, autorepeat, count);
        event.setTimestamp(timestamp);
        filtered = inputContext->filterEvent(&event);
    }

    if (!filtered) {
        auto window = focusWindow()->window();

        if (type == QEvent::KeyPress && key == Qt::Key_Menu) {
            auto cursor = window->screen()->handle()->cursor();
            if (cursor) {
                const QPoint globalPos = cursor->pos();
                const QPoint pos = window->mapFromGlobal(globalPos);
                QWindowSystemInterface::handleContextMenuEvent(window, false, pos, globalPos, modifiers);
            }
        }

        QWindowSystemInterface::handleExtendedKeyEvent(window, timestamp, type, key, modifiers,
                nativeScanCode, nativeVirtualKey, nativeModifiers, text, autorepeat, count);
    }
}

void QWaylandInputDevice::Keyboard::keyboard_key(uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    if (mKeymapFormat != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && mKeymapFormat != WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP) {
        qCWarning(lcQpaWayland) << Q_FUNC_INFO << "unknown keymap format:" << mKeymapFormat;
        return;
    }

    auto *window = focusWindow();
    if (!window) {
        // We destroyed the keyboard focus surface, but the server didn't get the message yet...
        // or the server didn't send an enter event first. In either case, ignore the event.
        return;
    }

    mParent->mSerial = serial;

    const bool isDown = state != WL_KEYBOARD_KEY_STATE_RELEASED;
    if (isDown)
        mParent->mQDisplay->setLastInputDevice(mParent, serial, window);

    if (mKeymapFormat == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
#if QT_CONFIG(xkbcommon)
        if ((!mXkbKeymap || !mXkbState) && !createDefaultKeymap())
            return;

        auto code = key + 8; // map to wl_keyboard::keymap_format::keymap_format_xkb_v1

        xkb_keysym_t sym = xkb_state_key_get_one_sym(mXkbState.get(), code);

        Qt::KeyboardModifiers modifiers = mParent->modifiers();

        int qtkey = QXkbCommon::keysymToQtKey(sym, modifiers, mXkbState.get(), code);
        QString text = QXkbCommon::lookupString(mXkbState.get(), code);

        QEvent::Type type = isDown ? QEvent::KeyPress : QEvent::KeyRelease;
        handleKey(time, type, qtkey, modifiers, code, sym, mNativeModifiers, text);

        if (state == WL_KEYBOARD_KEY_STATE_PRESSED && xkb_keymap_key_repeats(mXkbKeymap.get(), code) && mRepeatRate > 0) {
            mRepeatKey.key = qtkey;
            mRepeatKey.code = code;
            mRepeatKey.time = time;
            mRepeatKey.text = text;
            mRepeatKey.nativeVirtualKey = sym;
            mRepeatTimer.setInterval(mRepeatDelay);
            mRepeatTimer.start();
        } else if (mRepeatKey.code == code) {
            mRepeatTimer.stop();
        }
#else
        Q_UNUSED(time);
        Q_UNUSED(key);
        qCWarning(lcQpaWayland, "xkbcommon not available on this build, not performing key mapping");
        return;
#endif
    } else if (mKeymapFormat == WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP) {
        // raw scan code
        return;
    }
}

void QWaylandInputDevice::Keyboard::handleFocusDestroyed()
{
    // The signal is emitted by QWaylandWindow, which is not necessarily destroyed along with the
    // surface, so we still need to disconnect the signal
    auto *window = qobject_cast<QWaylandWindow *>(sender());
    disconnect(window, &QWaylandWindow::wlSurfaceDestroyed, this, &Keyboard::handleFocusDestroyed);
    Q_ASSERT(window->wlSurface() == mFocus);
    handleFocusLost();
}

void QWaylandInputDevice::Keyboard::handleFocusLost()
{
    mFocus = nullptr;
#if QT_CONFIG(clipboard)
    if (auto *dataDevice = mParent->dataDevice())
        dataDevice->invalidateSelectionOffer();
#endif
#if QT_CONFIG(wayland_client_primary_selection)
    if (auto *device = mParent->primarySelectionDevice())
        device->invalidateSelectionOffer();
#endif
    mParent->mQDisplay->handleKeyboardFocusChanged(mParent);
    mRepeatTimer.stop();
}

void QWaylandInputDevice::Keyboard::keyboard_modifiers(uint32_t serial,
                                             uint32_t mods_depressed,
                                             uint32_t mods_latched,
                                             uint32_t mods_locked,
                                             uint32_t group)
{
    Q_UNUSED(serial);
#if QT_CONFIG(xkbcommon)
    if (mXkbState)
        xkb_state_update_mask(mXkbState.get(),
                              mods_depressed, mods_latched, mods_locked,
                              0, 0, group);
    mNativeModifiers = mods_depressed | mods_latched | mods_locked;
#else
    Q_UNUSED(mods_depressed);
    Q_UNUSED(mods_latched);
    Q_UNUSED(mods_locked);
    Q_UNUSED(group);
#endif
}

void QWaylandInputDevice::Keyboard::keyboard_repeat_info(int32_t rate, int32_t delay)
{
    mRepeatRate = rate;
    mRepeatDelay = delay;
}

void QWaylandInputDevice::Touch::touch_down(uint32_t serial,
                                     uint32_t time,
                                     struct wl_surface *surface,
                                     int32_t id,
                                     wl_fixed_t x,
                                     wl_fixed_t y)
{
    if (!surface)
        return;

    auto *window = QWaylandWindow::fromWlSurface(surface);
    if (!window)
        return; // Ignore foreign surfaces

    mParent->mTime = time;
    mParent->mSerial = serial;
    mFocus = window;
    mParent->mQDisplay->setLastInputDevice(mParent, serial, mFocus);
    QPointF position(wl_fixed_to_double(x), wl_fixed_to_double(y));
    mParent->handleTouchPoint(id, Qt::TouchPointPressed, position);
}

void QWaylandInputDevice::Touch::touch_up(uint32_t serial, uint32_t time, int32_t id)
{
    Q_UNUSED(serial);
    mParent->mTime = time;
    mParent->handleTouchPoint(id, Qt::TouchPointReleased);

    if (allTouchPointsReleased()) {
        mFocus = nullptr;

        // As of Weston 7.0.0 there is no touch_frame after the last touch_up
        // (i.e. when the last finger is released). To accommodate for this, issue a
        // touch_frame. This cannot hurt since it is safe to call the touch_frame
        // handler multiple times when there are no points left.
        // See: https://gitlab.freedesktop.org/wayland/weston/issues/44
        // TODO: change logging category to lcQpaWaylandInput in newer versions.
        qCDebug(lcQpaWayland, "Generating fake frame event to work around Weston bug");
        touch_frame();
    }
}

void QWaylandInputDevice::Touch::touch_motion(uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    QPointF position(wl_fixed_to_double(x), wl_fixed_to_double(y));
    mParent->mTime = time;
    mParent->handleTouchPoint(id, Qt::TouchPointMoved, position);
}

void QWaylandInputDevice::Touch::touch_cancel()
{
    mPendingTouchPoints.clear();

    QWaylandTouchExtension *touchExt = mParent->mQDisplay->touchExtension();
    if (touchExt)
        touchExt->touchCanceled();

    QWindowSystemInterface::handleTouchCancelEvent(nullptr, mParent->mTouchDevice);
}

void QWaylandInputDevice::handleTouchPoint(int id, Qt::TouchPointState state, const QPointF &surfacePosition)
{
    auto end = mTouch->mPendingTouchPoints.end();
    auto it = std::find_if(mTouch->mPendingTouchPoints.begin(), end, [id](const QWindowSystemInterface::TouchPoint &tp){ return tp.id == id; });
    if (it == end) {
        it = mTouch->mPendingTouchPoints.insert(end, QWindowSystemInterface::TouchPoint());
        it->id = id;
    }
    // If the touch points were up and down in same frame, send out frame right away
    else if ((it->state == Qt::TouchPointPressed && state == Qt::TouchPointReleased)
            || (it->state == Qt::TouchPointReleased && state == Qt::TouchPointPressed)) {
        mTouch->touch_frame();
        it = mTouch->mPendingTouchPoints.insert(mTouch->mPendingTouchPoints.end(), QWindowSystemInterface::TouchPoint());
        it->id = id;
    }

    QWindowSystemInterface::TouchPoint &tp = *it;

    // Only moved and pressed needs to update/set position
    if (state == Qt::TouchPointMoved || state == Qt::TouchPointPressed) {
        // We need a global (screen) position.
        QWaylandWindow *win = mTouch->mFocus;

        //is it possible that mTouchFocus is null;
        if (!win && mPointer)
            win = mPointer->focusWindow();
        if (!win && mKeyboard)
            win = mKeyboard->focusWindow();
        if (!win || !win->window())
            return;

        tp.area = QRectF(0, 0, 8, 8);
        QPointF localPosition = win->mapFromWlSurface(surfacePosition);
        // TODO: This doesn't account for high dpi scaling for the delta, but at least it matches
        // what we have for mouse input.
        QPointF delta = localPosition - localPosition.toPoint();
        QPointF globalPosition = win->window()->mapToGlobal(localPosition.toPoint()) + delta;
        tp.area.moveCenter(globalPosition);
    }

    // If the touch point was pressed earlier this frame, we don't want to overwrite its state.
    if (tp.state != Qt::TouchPointPressed)
        tp.state = state;

    tp.pressure = tp.state == Qt::TouchPointReleased ? 0 : 1;
}

bool QWaylandInputDevice::Touch::allTouchPointsReleased()
{
    for (const auto &tp : qAsConst(mPendingTouchPoints)) {
        if (tp.state != Qt::TouchPointReleased)
            return false;
    }
    return true;
}

void QWaylandInputDevice::Touch::releasePoints()
{
    if (mPendingTouchPoints.empty())
        return;

    for (QWindowSystemInterface::TouchPoint &tp : mPendingTouchPoints)
        tp.state = Qt::TouchPointReleased;

    touch_frame();
}

void QWaylandInputDevice::Touch::touch_frame()
{
    // TODO: early return if no events?

    QWindow *window = mFocus ? mFocus->window() : nullptr;

    if (mFocus) {
        const QWindowSystemInterface::TouchPoint &tp = mPendingTouchPoints.last();
        // When the touch event is received, the global pos is calculated with the margins
        // in mind. Now we need to adjust again to get the correct local pos back.
        QMargins margins = window->frameMargins();
        QPoint p = tp.area.center().toPoint();
        QPointF localPos(window->mapFromGlobal(QPoint(p.x() + margins.left(), p.y() + margins.top())));
        if (mFocus->touchDragDecoration(mParent, localPos, tp.area.center(), tp.state, mParent->modifiers()))
            return;
    }

    QWindowSystemInterface::handleTouchEvent(window, mParent->mTime, mParent->mTouchDevice, mPendingTouchPoints, mParent->modifiers());

    // Prepare state for next frame
    const auto prevTouchPoints = mPendingTouchPoints;
    mPendingTouchPoints.clear();
    for (const auto &prevPoint: prevTouchPoints) {
        // All non-released touch points should be part of the next touch event
        if (prevPoint.state != Qt::TouchPointReleased) {
            QWindowSystemInterface::TouchPoint tp = prevPoint;
            tp.state = Qt::TouchPointStationary; // ... as stationary (unless proven otherwise)
            mPendingTouchPoints.append(tp);
        }
    }

}

}

QT_END_NAMESPACE

#include "moc_qwaylandinputdevice_p.cpp"
