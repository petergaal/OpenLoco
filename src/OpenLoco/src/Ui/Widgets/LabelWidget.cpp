#include "LabelWidget.h"
#include "Graphics/Colour.h"
#include "Graphics/ImageIds.h"
#include "Graphics/SoftwareDrawingEngine.h"
#include "Graphics/TextRenderer.h"
#include "Localisation/Formatting.h"
#include "Ui/Window.h"
#include <cassert>

namespace OpenLoco::Ui::Widgets
{
    // 0x004CB21D
    void Label::draw(Gfx::DrawingContext& drawingCtx, const Widget& widget, const WidgetState& widgetState)
    {
        if (widget.text == StringIds::null || widget.text == StringIds::empty)
        {
            return;
        }

        auto colour = widgetState.colour.opaque();
        if (widgetState.disabled)
        {
            colour = colour.inset();
        }

        const auto size = widget.size();

        auto formatArgs = FormatArguments(widget.textArgs);
        auto tr = Gfx::TextRenderer(drawingCtx);
        tr.setCurrentFont(widget.font);

        const auto x = [&]() -> int16_t {
            if (widget.contentAlign == ContentAlign::left)
            {
                return 0;
            }
            else if (widget.contentAlign == ContentAlign::center)
            {
                return (size.width - 1) / 2;
            }
            else if (widget.contentAlign == ContentAlign::right)
            {
                char buffer[512]{};
                StringManager::formatString(buffer, std::size(buffer), widget.text, formatArgs);
                const auto stringWidth = tr.getStringWidthNewLined(buffer);
                return size.width - stringWidth - 1;
            }
            assert(false);
            return {};
        }();

        const auto fontHeight = tr.getLineHeight(tr.getCurrentFont());
        // NOTE: -1 is an ugly hack for buttons with inset border, remove that when all buttons have consistent height.
        const int16_t yOffset = std::max<int16_t>(0, (size.height - fontHeight) / 2 - 1);
        const int16_t y = yOffset;
        const int16_t width = size.width - 2;

        if (widget.contentAlign == ContentAlign::left)
        {
            tr.drawStringLeftClipped(Point(x, y), width, colour, widget.text, formatArgs);
        }
        else if (widget.contentAlign == ContentAlign::center)
        {
            tr.drawStringCentredClipped(Point(x, y), width, colour, widget.text, formatArgs);
        }
        else if (widget.contentAlign == ContentAlign::right)
        {
            tr.drawStringLeftClipped(Point(x, y), width, colour, widget.text, formatArgs);
        }
    }
}
