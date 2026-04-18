/*
 * AquaAssets - Singleton class that loads Snow Leopard scrollbar PNG assets.
 *
 * This class loads all the scrollbar images once at startup and provides
 * accessor methods so the style rendering code can draw them. The images
 * are loaded from ~/.local/share/aqua-scrollbar/ at runtime.
 *
 * The scrollbar is drawn using a 3-piece pattern:
 *   top cap -> stretchable center -> bottom cap
 * This gives us pixel-perfect Snow Leopard scrollbars.
 */

#pragma once

#include <QPixmap>
#include <QDir>
#include <QString>
#include <QFile>
#include <QTextStream>

namespace Breeze
{

class AquaAssets
{
public:
    // Get the single shared instance of AquaAssets.
    // The first time this is called, it loads all the PNGs from disk.
    static AquaAssets& instance()
    {
        static AquaAssets s;
        return s;
    }

    // --- Scrollbar handle (thumb) pieces ---
    // Normal state
    const QPixmap& thumbTop() const { return m_thumbTop; }
    const QPixmap& thumbCenter() const { return m_thumbCenter; }
    const QPixmap& thumbBottom() const { return m_thumbBottom; }

    // Pressed state (when user is actively dragging the handle)
    const QPixmap& thumbPressedTop() const { return m_thumbPressedTop; }
    const QPixmap& thumbPressedCenter() const { return m_thumbPressedCenter; }
    const QPixmap& thumbPressedBottom() const { return m_thumbPressedBottom; }

    // --- Track (groove) pieces ---
    const QPixmap& trackTop() const { return m_trackTop; }
    const QPixmap& trackCenter() const { return m_trackCenter; }
    const QPixmap& trackBottom() const { return m_trackBottom; }

    // --- Arrow buttons ---
    // Normal state
    const QPixmap& arrowUp() const { return m_arrowUp; }
    const QPixmap& arrowDown() const { return m_arrowDown; }

    // Pressed state
    const QPixmap& arrowUpPressed() const { return m_arrowUpPressed; }
    const QPixmap& arrowDownPressed() const { return m_arrowDownPressed; }

    // --- Checkbox assets (from real Snow Leopard HITheme render) ---
    const QPixmap& checkboxOff() const { return m_checkboxOff; }
    const QPixmap& checkboxOn() const { return m_checkboxOn; }
    const QPixmap& checkboxMixed() const { return m_checkboxMixed; }
    const QPixmap& checkboxPressed() const { return m_checkboxPressed; }
    const QPixmap& checkboxPressedOn() const { return m_checkboxPressedOn; }
    const QPixmap& checkboxDisabled() const { return m_checkboxDisabled; }

    // --- Radio button assets ---
    const QPixmap& radioOff() const { return m_radioOff; }
    const QPixmap& radioOn() const { return m_radioOn; }
    const QPixmap& radioPressed() const { return m_radioPressed; }
    const QPixmap& radioPressedOn() const { return m_radioPressedOn; }
    const QPixmap& radioDisabled() const { return m_radioDisabled; }

    // --- Push button assets (3-piece stretch) ---
    const QPixmap& pushLeft() const { return m_pushLeft; }
    const QPixmap& pushCenter() const { return m_pushCenter; }
    const QPixmap& pushRight() const { return m_pushRight; }
    const QPixmap& pushDefaultLeft() const { return m_pushDefaultLeft; }
    const QPixmap& pushDefaultCenter() const { return m_pushDefaultCenter; }
    const QPixmap& pushDefaultRight() const { return m_pushDefaultRight; }
    const QPixmap& pushPressedLeft() const { return m_pushPressedLeft; }
    const QPixmap& pushPressedCenter() const { return m_pushPressedCenter; }
    const QPixmap& pushPressedRight() const { return m_pushPressedRight; }

    // --- Progress bar assets ---
    const QPixmap& progressTrackLeft() const { return m_progressTrackLeft; }
    const QPixmap& progressTrackCenter() const { return m_progressTrackCenter; }
    const QPixmap& progressTrackRight() const { return m_progressTrackRight; }
    const QPixmap& progressFillLeft() const { return m_progressFillLeft; }
    const QPixmap& progressFillCenter() const { return m_progressFillCenter; }
    const QPixmap& progressFillRight() const { return m_progressFillRight; }

    // Returns true if all assets loaded successfully
    bool isValid() const { return m_valid; }

private:
    // Private constructor - loads all assets from the runtime directory.
    // The path is ~/.local/share/aqua-scrollbar/ so that assets survive
    // SteamOS updates (which wipe /usr but not the home directory).
    AquaAssets()
    {
        const QString dir = QDir::homePath() + QStringLiteral("/.local/share/aqua-scrollbar/");

        m_thumbTop    = QPixmap(dir + QStringLiteral("ecsbl_thumb_top.png"));
        m_thumbCenter = QPixmap(dir + QStringLiteral("ecsbl_thumb_center.png"));
        m_thumbBottom = QPixmap(dir + QStringLiteral("ecsbl_thumb_bottom.png"));

        m_thumbPressedTop    = QPixmap(dir + QStringLiteral("ecsbl_thumb_pressed_top.png"));
        m_thumbPressedCenter = QPixmap(dir + QStringLiteral("ecsbl_thumb_pressed_center.png"));
        m_thumbPressedBottom = QPixmap(dir + QStringLiteral("ecsbl_thumb_pressed_bottom.png"));

        m_trackTop    = QPixmap(dir + QStringLiteral("ecsbl_track_top.png"));
        m_trackCenter = QPixmap(dir + QStringLiteral("ecsbl_track_center.png"));
        m_trackBottom = QPixmap(dir + QStringLiteral("ecsbl_track_bottom.png"));

        m_arrowUp          = QPixmap(dir + QStringLiteral("scrollarrow-u.png"));
        m_arrowDown        = QPixmap(dir + QStringLiteral("scrollarrow-d.png"));
        m_arrowUpPressed   = QPixmap(dir + QStringLiteral("scrollarrow-up.png"));
        m_arrowDownPressed = QPixmap(dir + QStringLiteral("scrollarrow-dp.png"));

        // Load widget assets from separate directory
        const QString wdir = QDir::homePath() + QStringLiteral("/.local/share/aqua-widgets/");
        m_checkboxOff      = QPixmap(wdir + QStringLiteral("checkbox_active_regular.png"));
        m_checkboxOn       = QPixmap(wdir + QStringLiteral("checkbox_active_on_regular.png"));
        m_checkboxMixed    = QPixmap(wdir + QStringLiteral("checkbox_active_mixed_regular.png"));
        m_checkboxPressed  = QPixmap(wdir + QStringLiteral("checkbox_pressed_regular.png"));
        m_checkboxPressedOn= QPixmap(wdir + QStringLiteral("checkbox_pressed_on_regular.png"));
        m_checkboxDisabled = QPixmap(wdir + QStringLiteral("checkbox_disabled_regular.png"));

        m_radioOff         = QPixmap(wdir + QStringLiteral("radio_active_regular.png"));
        m_radioOn          = QPixmap(wdir + QStringLiteral("radio_active_on_regular.png"));
        m_radioPressed     = QPixmap(wdir + QStringLiteral("radio_pressed_regular.png"));
        m_radioPressedOn   = QPixmap(wdir + QStringLiteral("radio_pressed_on_regular.png"));
        m_radioDisabled    = QPixmap(wdir + QStringLiteral("radio_disabled_regular.png"));

        // Push button pieces (3-piece stretch)
        m_pushLeft          = QPixmap(wdir + QStringLiteral("push_active_regular_left.png"));
        m_pushCenter        = QPixmap(wdir + QStringLiteral("push_active_regular_center.png"));
        m_pushRight         = QPixmap(wdir + QStringLiteral("push_active_regular_right.png"));
        m_pushDefaultLeft   = QPixmap(wdir + QStringLiteral("push_active_regular_default_left.png"));
        m_pushDefaultCenter = QPixmap(wdir + QStringLiteral("push_active_regular_default_center.png"));
        m_pushDefaultRight  = QPixmap(wdir + QStringLiteral("push_active_regular_default_right.png"));
        m_pushPressedLeft   = QPixmap(wdir + QStringLiteral("push_pressed_regular_left.png"));
        m_pushPressedCenter = QPixmap(wdir + QStringLiteral("push_pressed_regular_center.png"));
        m_pushPressedRight  = QPixmap(wdir + QStringLiteral("push_pressed_regular_right.png"));

        // Progress bar pieces
        m_progressTrackLeft   = QPixmap(wdir + QStringLiteral("progress_track_left.png"));
        m_progressTrackCenter = QPixmap(wdir + QStringLiteral("progress_track_center.png"));
        m_progressTrackRight  = QPixmap(wdir + QStringLiteral("progress_track_right.png"));
        m_progressFillLeft    = QPixmap(wdir + QStringLiteral("progress_fill_left.png"));
        m_progressFillCenter  = QPixmap(wdir + QStringLiteral("progress_fill_center.png"));
        m_progressFillRight   = QPixmap(wdir + QStringLiteral("progress_fill_right.png"));

        // Check that at least the basic pieces loaded
        m_valid = !m_thumbTop.isNull() && !m_thumbCenter.isNull()
               && !m_thumbBottom.isNull() && !m_trackTop.isNull()
               && !m_trackCenter.isNull() && !m_trackBottom.isNull();

        // Debug: write detailed load status with pixel colors
        QFile dbg(dir + QStringLiteral("debug.txt"));
        if (dbg.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&dbg);
            ts << "dir: " << dir << "\n";
            ts << "valid: " << m_valid << "\n";
            ts << "thumbTop: " << m_thumbTop.width() << "x" << m_thumbTop.height()
               << " alpha=" << m_thumbTop.hasAlphaChannel() << "\n";
            ts << "thumbCenter: " << m_thumbCenter.width() << "x" << m_thumbCenter.height()
               << " alpha=" << m_thumbCenter.hasAlphaChannel() << "\n";
            // Sample the actual pixel color of the loaded thumb center
            QImage tImg = m_thumbCenter.toImage();
            if (!tImg.isNull() && tImg.width() > 0) {
                QRgb px = tImg.pixel(tImg.width()/2, 0);
                ts << "thumbCenter_pixel: r=" << qRed(px) << " g=" << qGreen(px)
                   << " b=" << qBlue(px) << " a=" << qAlpha(px) << "\n";
            }
            QImage trImg = m_trackCenter.toImage();
            if (!trImg.isNull() && trImg.width() > 0) {
                QRgb px = trImg.pixel(trImg.width()/2, 0);
                ts << "trackCenter_pixel: r=" << qRed(px) << " g=" << qGreen(px)
                   << " b=" << qBlue(px) << " a=" << qAlpha(px) << "\n";
            }
            dbg.close();
        }
    }

    // Prevent copying
    AquaAssets(const AquaAssets&) = delete;
    AquaAssets& operator=(const AquaAssets&) = delete;

    // The loaded pixmaps
    QPixmap m_thumbTop, m_thumbCenter, m_thumbBottom;
    QPixmap m_thumbPressedTop, m_thumbPressedCenter, m_thumbPressedBottom;
    QPixmap m_trackTop, m_trackCenter, m_trackBottom;
    QPixmap m_arrowUp, m_arrowDown, m_arrowUpPressed, m_arrowDownPressed;
    // Widget assets
    QPixmap m_checkboxOff, m_checkboxOn, m_checkboxMixed;
    QPixmap m_checkboxPressed, m_checkboxPressedOn, m_checkboxDisabled;
    QPixmap m_radioOff, m_radioOn, m_radioPressed, m_radioPressedOn, m_radioDisabled;
    // Push button
    QPixmap m_pushLeft, m_pushCenter, m_pushRight;
    QPixmap m_pushDefaultLeft, m_pushDefaultCenter, m_pushDefaultRight;
    QPixmap m_pushPressedLeft, m_pushPressedCenter, m_pushPressedRight;
    // Progress bar
    QPixmap m_progressTrackLeft, m_progressTrackCenter, m_progressTrackRight;
    QPixmap m_progressFillLeft, m_progressFillCenter, m_progressFillRight;
    bool m_valid = false;
};

} // namespace Breeze
