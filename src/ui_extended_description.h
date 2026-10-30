#pragma once
#ifndef CATA_SRC_UI_EXTENDED_DESCRIPTION_H
#define CATA_SRC_UI_EXTENDED_DESCRIPTION_H

#include <string>
#include <vector>

#include "coordinates.h"
#include "input_context.h"
#include "output.h"
#include "rml_window.h"

enum class description_target : int {
    creature = 0,
    furniture,
    terrain,
    vehicle,
    num_targets
};

/// An extended description as RML: each entry a paragraph, and "--" a separator.
std::string extended_description_rml( const std::vector<std::string> &description );

class extended_description_window : public rml_ui::window
{
    public:
        explicit extended_description_window( tripoint_bub_ms &p );

        void show();

    protected:
        void on_redraw() override;

    private:
        std::string tab_rml() const;

        input_context ctxt;
        description_target cur_target = description_target::terrain;
        // The tab the body was last filled for, to scroll a new one to its top
        description_target shown_target = description_target::num_targets;
        tripoint_bub_ms p;

        std::vector<std::string> creature_description;
        std::vector<std::string> furniture_description;
        std::vector<std::string> terrain_description;
        std::vector<std::string> veh_app_description;
};

#endif // CATA_SRC_UI_EXTENDED_DESCRIPTION_H
