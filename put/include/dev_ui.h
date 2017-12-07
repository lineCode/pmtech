#ifndef _DEV_UI_H
#define _DEV_UI_H

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "pen.h"
#include "dev_ui_icons.h"
#include "pen_json.h"

namespace put
{
	namespace dev_ui
	{
        enum io_capture : u32
        {
            MOUSE      = 1<<0,
            KAYBOARD   = 1<<1,
            TEXT       = 1<<2
        };
        
        enum e_file_browser_flags : u32
        {
            FB_OPEN = 0,
            FB_SAVE = 1,
        };
        
        enum e_console_log_level : u32
        {
            CONSOLE_MESSAGE = 0,
            CONSOLE_WARNING = 1,
            CONSOLE_ERROR = 2
        };
        
		//imgui_renderer
		bool init();
		void shutdown();
		void new_frame();
		void render();
        u32 want_capture();
        
        void util_init();
        
        //console
        void show_console( bool val );
        void log( const c8* fmt, ... );
        void log_level( u32 level, const c8* fmt, ... );
        void console();
        
        //imgui extensions
        bool state_button( const c8* text, bool state_active );
        void set_tooltip( const c8* fmt, ... );
        const c8* file_browser(bool& dialog_open, u32 flags, s32 num_filetypes = 0, ...);
        
        //generic program preferences
        void set_program_preference( const c8* name, Str val );
        pen::json get_program_preference( const c8* name );
	}
}

#endif
