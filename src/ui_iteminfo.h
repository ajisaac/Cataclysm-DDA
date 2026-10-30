#pragma once
#ifndef CATA_SRC_UI_ITEMINFO_H
#define CATA_SRC_UI_ITEMINFO_H

#include "input_context.h"
#include "output.h"
#include "point.h"
#include "rml_window.h"

/// An item's info in a window of its own.
class iteminfo_window : public rml_ui::window
{
    public:
        /**
         * @param pos Top left corner, in terminal cells; a negative coordinate
         *            centers the window on that axis.
         * @param width Width in terminal cells.
         * @param height Height in terminal cells, or 0 to fit the content.
         */
        iteminfo_window( item_info_data &info, point pos, int width, int height );
        void execute();

    protected:
        void on_redraw() override;

    private:
        item_info_data data;
        point pos;
        int width;
        int height;
        input_context ctxt;
};

#endif // CATA_SRC_UI_ITEMINFO_H
