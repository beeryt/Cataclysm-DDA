#include "messages.h"

#include "calendar.h"
#include "compatibility.h" // needed for the workaround for the std::to_string bug in some compilers
#include "debug.h"
#include "game.h"
#include "input.h"
#include "json.h"
#include "output.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translations.h"

#ifdef __ANDROID__
#include "options.h"
#include "SDL_keyboard.h"
#endif

#include <deque>
#include <iterator>
#include <algorithm>

// sidebar messages flow direction
extern bool log_from_top;
extern int message_ttl;

namespace
{

struct game_message : public JsonDeserializer, public JsonSerializer {
    std::string       message;
    time_point timestamp_in_turns  = 0;
    int               timestamp_in_user_actions = 0;
    int               count = 1;
    game_message_type type  = m_neutral;

    game_message() = default;
    game_message( std::string &&msg, game_message_type const t ) :
        message( std::move( msg ) ),
        timestamp_in_turns( calendar::turn ),
        timestamp_in_user_actions( g->get_user_action_counter() ),
        type( t ) {
    }

    const time_point &turn() const {
        return timestamp_in_turns;
    }

    std::string get_with_count() const {
        if( count <= 1 ) {
            return message;
        }
        //~ Message %s on the message log was repeated %d times, e.g. "You hear a whack! x 12"
        return string_format( _( "%s x %d" ), message.c_str(), count );
    }

    bool is_new( const time_point &current ) const {
        return turn() >= current;
    }

    bool is_recent( const time_point &current ) const {
        return turn() + 5_turns >= current;
    }

    nc_color get_color( const time_point &current ) const {
        if( is_new( current ) ) {
            // color for new messages
            return msgtype_to_color( type, false );

        } else if( is_recent( current ) ) {
            // color for slightly old messages
            return msgtype_to_color( type, true );
        }

        // color for old messages
        return c_dark_gray;
    }

    void deserialize( JsonIn &jsin ) override {
        JsonObject obj = jsin.get_object();
        obj.read( "turn", timestamp_in_turns );
        message = obj.get_string( "message" );
        count = obj.get_int( "count" );
        type = static_cast<game_message_type>( obj.get_int( "type" ) );
    }

    void serialize( JsonOut &jsout ) const override {
        jsout.start_object();
        jsout.member( "turn", timestamp_in_turns );
        jsout.member( "message", message );
        jsout.member( "count", count );
        jsout.member( "type", static_cast<int>( type ) );
        jsout.end_object();
    }
};

class messages_impl
{
    public:
        std::deque<game_message> messages;   // Messages to be printed
        time_point curmes = 0; // The last-seen message.
        bool active = true;

        bool has_undisplayed_messages() const {
            return !messages.empty() && messages.back().turn() > curmes;
        }

        game_message const &history( int const i ) const {
            return messages[messages.size() - i - 1];
        }

        // coalesce recent like messages
        bool coalesce_messages( std::string const &msg, game_message_type const type ) {
            if( messages.empty() ) {
                return false;
            }

            auto &last_msg = messages.back();
            if( last_msg.turn() + 3_turns < calendar::turn ) {
                return false;
            }

            if( type != last_msg.type || msg != last_msg.message ) {
                return false;
            }

            last_msg.count++;
            last_msg.timestamp_in_turns = calendar::turn;
            last_msg.timestamp_in_user_actions = g->get_user_action_counter();
            last_msg.type = type;

            return true;
        }

        void add_msg_string( std::string &&msg ) {
            add_msg_string( std::move( msg ), m_neutral );
        }

        void add_msg_string( std::string &&msg, game_message_type const type ) {
            if( msg.length() == 0 || !active ) {
                return;
            }

            if( type == m_debug && !debug_mode ) {
                return;
            }

            if( coalesce_messages( msg, type ) ) {
                return;
            }

            while( messages.size() > 255 ) {
                messages.pop_front();
            }

            messages.emplace_back( std::move( msg ), type );
        }

        std::vector<std::pair<std::string, std::string>> recent_messages( size_t count ) const {
            count = std::min( count, messages.size() );

            std::vector<std::pair<std::string, std::string>> result;
            result.reserve( count );

            const int offset = static_cast<std::ptrdiff_t>( messages.size() - count );

            std::transform( begin( messages ) + offset, end( messages ), back_inserter( result ),
            []( game_message const & msg ) {
                return std::make_pair( to_string_time_of_day( msg.timestamp_in_turns ),
                                       msg.get_with_count() );
            } );

            return result;
        }
};

// Messages object.
messages_impl player_messages;

bool message_exceeds_ttl( const game_message &message )
{
    return message_ttl > 0 &&
           message.timestamp_in_user_actions + message_ttl <= g->get_user_action_counter();
}

} //namespace

std::vector<std::pair<std::string, std::string>> Messages::recent_messages( const size_t count )
{
    return player_messages.recent_messages( count );
}

void Messages::serialize( JsonOut &json )
{
    json.member( "player_messages" );
    json.start_object();
    json.member( "messages", player_messages.messages );
    json.member( "curmes", player_messages.curmes );
    json.end_object();
}

void Messages::deserialize( JsonObject &json )
{
    if( !json.has_member( "player_messages" ) ) {
        return;
    }

    JsonObject obj = json.get_object( "player_messages" );
    obj.read( "messages", player_messages.messages );
    obj.read( "curmes", player_messages.curmes );
}

void Messages::add_msg( std::string msg )
{
    player_messages.add_msg_string( std::move( msg ) );
}

void Messages::add_msg( const game_message_type type, std::string msg )
{
    player_messages.add_msg_string( std::move( msg ), type );
}

void Messages::clear_messages()
{
    player_messages.messages.clear();
    player_messages.active = true;
}

void Messages::deactivate()
{
    player_messages.active = false;
}

size_t Messages::size()
{
    return player_messages.messages.size();
}

bool Messages::has_undisplayed_messages()
{
    return player_messages.has_undisplayed_messages();
}

// Returns pairs of message log type id and untranslated name
static const std::vector<std::pair<game_message_type, const char *>> &msg_type_and_names()
{
    static const std::vector<std::pair<game_message_type, const char *>> type_n_names = {
        { m_good, translate_marker_context( "message type", "good" ) },
        { m_bad, translate_marker_context( "message type", "bad" ) },
        { m_mixed, translate_marker_context( "message type", "mixed" ) },
        { m_warning, translate_marker_context( "message type", "warning" ) },
        { m_info, translate_marker_context( "message type", "info" ) },
        { m_neutral, translate_marker_context( "message type", "neutral" ) },
        { m_debug, translate_marker_context( "message type", "debug" ) },
    };
    return type_n_names;
}

// Get message type from translated name, returns true if name is a valid translated name
static bool msg_type_from_name( game_message_type &type, const std::string &name )
{
    for( const auto &p : msg_type_and_names() ) {
        if( name == pgettext( "message type", p.second ) ) {
            type = p.first;
            return true;
        }
    }
    return false;
}

namespace Messages
{
class dialog
{
    public:
        dialog();
        void run();
    private:
        void init();
        void show();
        void input();
        void do_filter( const std::string &filter_str );
        static std::vector<std::string> filter_help_text( int width );

        const nc_color border_color;
        const nc_color filter_color;
        const nc_color time_color;
        const nc_color bracket_color;
        const nc_color filter_help_color;

        // border_width padding_width         border_width
        //      v           v                     v
        //
        //      | 12 seconds Never mind. x 2      |
        //
        //       '-----v---' '---------v---------'
        //        time_width       msg_width
        static constexpr int border_width = 1;
        static constexpr int padding_width = 1;
        int time_width, msg_width;

        size_t max_lines; // Max number of lines the window can show at once

        int w_x, w_y, w_width, w_height; // Main window position
        catacurses::window w; // Main window

        int w_fh_x, w_fh_y, w_fh_width, w_fh_height; // Filter help window position
        catacurses::window w_filter_help; // Filter help window

        std::vector<std::string> help_text; // Folded filter help text

        string_input_popup filter;
        bool filtering;
        std::string filter_str;

        input_context ctxt;

        // Message indices and folded strings
        std::vector<std::pair<size_t, std::string>> folded_all;
        // Indices of filtered messages
        std::vector<size_t> folded_filtered;

        size_t offset; // Index of the first printed message

        bool canceled;
        bool errored;
};
}

Messages::dialog::dialog()
    : border_color( BORDER_COLOR ), filter_color( c_white ),
      time_color( c_light_blue ), bracket_color( c_dark_gray ),
      filter_help_color( c_cyan )
{
    init();
}

void Messages::dialog::init()
{
    w_width = std::min( TERMX, FULL_SCREEN_WIDTH );
    w_height = std::min( TERMY, FULL_SCREEN_HEIGHT );
    w_x = ( TERMX - w_width ) / 2;
    w_y = ( TERMY - w_height ) / 2;

    w = catacurses::newwin( w_height, w_width, w_y, w_x );

    ctxt = input_context( "MESSAGE_LOG" );
    ctxt.register_action( "UP", _( "Scroll up" ) );
    ctxt.register_action( "DOWN", _( "Scroll down" ) );
    ctxt.register_action( "PAGE_UP" );
    ctxt.register_action( "PAGE_DOWN" );
    ctxt.register_action( "FILTER" );
    ctxt.register_action( "RESET_FILTER" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );

    // Calculate time string display width. The translated strings are expected to
    // be aligned, so we choose an arbitrary duration here to calculate the width.
    time_width = utf8_width( to_string_clipped( 1_turns, clipped_align::right ) );

    if( border_width * 2 + time_width + padding_width >= w_width ||
        border_width * 2 >= w_height ) {

        debugmsg( "No enough space for the message window" );
        errored = true;
        return;
    }
    msg_width = w_width - border_width * 2 - time_width - padding_width;
    max_lines = static_cast<size_t>( w_height - border_width * 2 );

    // Initialize filter help text and window
    w_fh_width = w_width;
    w_fh_x = w_x;
    help_text = filter_help_text( w_fh_width - border_width * 2 );
    w_fh_height = help_text.size() + border_width * 2;
    w_fh_y = w_y + w_height - w_fh_height;
    w_filter_help = catacurses::newwin( w_fh_height, w_fh_width, w_fh_y, w_fh_x );

    // Initialize filter input
    filter.window( w_filter_help, border_width + 2, w_fh_height - 1, w_fh_width - border_width - 2 );
    filtering = false;

    // Initialize folded messages
    folded_all.clear();
    folded_filtered.clear();
    const size_t msg_count = size();
    for( size_t ind = 0; ind < msg_count; ++ind ) {
        const size_t msg_ind = log_from_top ? ind : msg_count - 1 - ind;
        const game_message &msg = player_messages.history( msg_ind );
        const auto &folded = foldstring( msg.get_with_count(), msg_width );
        for( auto it = folded.begin(); it != folded.end(); ++it ) {
            folded_filtered.emplace_back( folded_all.size() );
            folded_all.emplace_back( msg_ind, *it );
        }
    }

    // Initialize scrolling offset
    if( log_from_top || max_lines > folded_filtered.size() ) {
        offset = 0;
    } else {
        offset = folded_filtered.size() - max_lines;
    }

    canceled = false;
    errored = false;
}

void Messages::dialog::show()
{
    werase( w );
    draw_border( w, border_color );

    scrollbar()
    .offset_x( 0 )
    .offset_y( border_width )
    .content_size( folded_filtered.size() )
    .viewport_pos( offset )
    .viewport_size( max_lines )
    .apply( w );

    // Range of window lines to print
    size_t line_from = 0, line_to;
    if( offset < folded_filtered.size() ) {
        line_to = std::min( max_lines, folded_filtered.size() - offset );
    } else {
        line_to = 0;
    }

    if( !log_from_top ) {
        // Always print from new to old
        std::swap( line_from, line_to );
    }
    std::string prev_time_str;
    bool printing_range = false;
    for( size_t line = line_from; line != line_to; ) {
        // Decrement here if printing from bottom to get the correct line number
        if( !log_from_top ) {
            --line;
        }

        const size_t folded_ind = offset + line;
        const size_t msg_ind = folded_all[folded_filtered[folded_ind]].first;
        const game_message &msg = player_messages.history( msg_ind );

        nc_color col = msgtype_to_color( msg.type, false );

        // Print current line
        print_colored_text( w, border_width + line, border_width + time_width + padding_width,
                            col, col, folded_all[folded_filtered[folded_ind]].second );

        // Generate aligned time string
        const time_point msg_time = msg.timestamp_in_turns;
        const std::string time_str = to_string_clipped( calendar::turn - msg_time, clipped_align::right );

        if( time_str != prev_time_str ) {
            // Time changed, print time string
            prev_time_str = time_str;
            right_print( w, border_width + line, border_width + msg_width + padding_width,
                         time_color, time_str );
            printing_range = false;
        } else {
            // Print line brackets to mark ranges of time
            if( printing_range ) {
                const size_t last_line = log_from_top ? line - 1 : line + 1;
                wattron( w, bracket_color );
                mvwaddch( w, border_width + last_line, border_width + time_width - 1, LINE_XOXO );
                wattroff( w, bracket_color );
            }
            wattron( w, bracket_color );
            mvwaddch( w, border_width + line, border_width + time_width - 1,
                      log_from_top ? LINE_XXOO : LINE_OXXO );
            wattroff( w, bracket_color );
            printing_range = true;
        }

        // Decrement for !log_from_top is done at the beginning
        if( log_from_top ) {
            ++line;
        }
    }

    if( filtering ) {
        wrefresh( w );
        // Print the help text
        werase( w_filter_help );
        draw_border( w_filter_help, border_color );
        for( size_t line = 0; line < help_text.size(); ++line ) {
            nc_color col = filter_help_color;
            print_colored_text( w_filter_help, border_width + line, border_width, col, col,
                                help_text[line] );
        }
        mvwprintz( w_filter_help, w_fh_height - 1, border_width, border_color, "< " );
        mvwprintz( w_filter_help, w_fh_height - 1, w_fh_width - border_width - 2, border_color, " >" );
        wrefresh( w_filter_help );

        // This line is preventing this method from being const
        filter.query( false, true ); // Draw only
    } else {
        if( filter_str.empty() ) {
            mvwprintz( w, w_height - 1, border_width, border_color, _( "< Press %s to filter, %s to reset >" ),
                       ctxt.get_desc( "FILTER" ), ctxt.get_desc( "RESET_FILTER" ) );
        } else {
            mvwprintz( w, w_height - 1, border_width, border_color, "< %s >", filter_str );
            mvwprintz( w, w_height - 1, border_width + 2, filter_color, "%s", filter_str );
        }
        wrefresh( w );
    }
}

void Messages::dialog::do_filter( const std::string &filter_str )
{
    // Split the search string into type and text
    bool has_type_filter = false;
    game_message_type filter_type = m_neutral;
    std::string filter_text;
    const auto colon = filter_str.find( ':' );
    if( colon != std::string::npos ) {
        has_type_filter = msg_type_from_name( filter_type, filter_str.substr( 0, colon ) );
        filter_text = filter_str.substr( colon + 1 );
    } else {
        filter_text = filter_str;
    }

    // Start filtering the log
    folded_filtered.clear();
    for( size_t folded_ind = 0; folded_ind < folded_all.size(); ) {
        const size_t msg_ind = folded_all[folded_ind].first;
        const game_message &msg = player_messages.history( msg_ind );
        const bool match = ( !has_type_filter || filter_type == msg.type ) &&
                           ci_find_substr( remove_color_tags( msg.get_with_count() ), filter_text ) >= 0;

        // Always advance the index, but only add to filtered list if the original message matches
        for( ; folded_ind < folded_all.size() && folded_all[folded_ind].first == msg_ind; ++folded_ind ) {
            if( match ) {
                folded_filtered.emplace_back( folded_ind );
            }
        }
    }

    // Reset view
    if( log_from_top || max_lines > folded_filtered.size() ) {
        offset = 0;
    } else {
        offset = folded_filtered.size() - max_lines;
    }
}

void Messages::dialog::input()
{
    canceled = false;
    if( filtering ) {
        filter.query( false );
        if( filter.confirmed() || filter.canceled() ) {
            filtering = false;
        }
        if( !filter.canceled() ) {
            const std::string &new_filter_str = filter.text();
            if( new_filter_str != filter_str ) {
                filter_str = new_filter_str;

                do_filter( filter_str );
            }
        } else {
            filter.text( filter_str );
        }
    } else {
        const std::string &action = ctxt.handle_input();
        if( action == "DOWN" && offset + max_lines < folded_filtered.size() ) {
            ++offset;
        } else if( action == "UP" && offset > 0 ) {
            --offset;
        } else if( action == "PAGE_DOWN" ) {
            if( offset + max_lines * 2 <= folded_filtered.size() ) {
                offset += max_lines;
            } else if( max_lines <= folded_filtered.size() ) {
                offset = folded_filtered.size() - max_lines;
            } else {
                offset = 0;
            }
        } else if( action == "PAGE_UP" ) {
            if( offset >= max_lines ) {
                offset -= max_lines;
            } else {
                offset = 0;
            }
        } else if( action == "FILTER" ) {
            filtering = true;
#ifdef __ANDROID__
            if( get_option<bool>( "ANDROID_AUTO_KEYBOARD" ) ) {
                SDL_StartTextInput();
            }
#endif
        } else if( action == "RESET_FILTER" ) {
            filter_str.clear();
            filter.text( filter_str );
            do_filter( filter_str );
        } else if( action == "QUIT" ) {
            canceled = true;
        }
    }
}

void Messages::dialog::run()
{
    while( !errored && !canceled ) {
        show();
        input();
    }
}

std::vector<std::string> Messages::dialog::filter_help_text( int width )
{
    const auto &help_fmt = _(
                               "Format is [[TYPE]:]TEXT. The values for TYPE are: %s\n"
                               "Examples:\n"
                               "  good:mutation\n"
                               "  :you pick up: 1\n"
                               "  crash!\n"
                           );
    std::stringstream type_text;
    const auto &type_list = msg_type_and_names();
    for( auto it = type_list.begin(); it != type_list.end(); ++it ) {
        // Skip m_debug outside debug mode (but allow searching for it)
        if( debug_mode || it->first != m_debug ) {
            const auto &col_name = get_all_colors().get_name( msgtype_to_color( it->first ) );
            auto next_it = std::next( it );
            // Skip m_debug outside debug mode
            if( !debug_mode && next_it != type_list.end() && next_it->first == m_debug ) {
                next_it = std::next( next_it );
            }
            if( next_it != type_list.end() ) {
                //~ the 2nd %s is a type name, this is used to format a list of type names
                type_text << string_format( pgettext( "message log", "<color_%s>%s</color>, " ),
                                            col_name, pgettext( "message type", it->second ) );
            } else {
                //~ the 2nd %s is a type name, this is used to format the last type name in a list of type names
                type_text << string_format( pgettext( "message log", "<color_%s>%s</color>." ),
                                            col_name, pgettext( "message type", it->second ) );
            }
        }
    }
    return foldstring( string_format( help_fmt, type_text.str() ), width );
}

void Messages::display_messages()
{
    dialog dlg;
    dlg.run();
    player_messages.curmes = calendar::turn;
}

void Messages::display_messages( const catacurses::window &ipk_target, int const left,
                                 int const top, int const right, int const bottom )
{
    if( !size() ) {
        return;
    }

    int const maxlength = right - left;
    int line = log_from_top ? top : bottom;

    if( log_from_top ) {
        for( int i = size() - 1; i >= 0; --i ) {
            if( line > bottom ) {
                break;
            }

            const game_message &m = player_messages.messages[i];
            if( message_exceeds_ttl( m ) ) {
                break;
            }

            const nc_color col = m.get_color( player_messages.curmes );
            std::string message_text = m.get_with_count();
            if( !m.is_recent( player_messages.curmes ) ) {
                message_text = remove_color_tags( message_text );
            }

            for( const std::string &folded : foldstring( message_text, maxlength ) ) {
                if( line > bottom ) {
                    break;
                }
                // Redrawing line to ensure new messages similar to previous
                // messages will not be missed by screen readers
                wredrawln( ipk_target, line, 1 );
                nc_color col_out = col;
                print_colored_text( ipk_target, line++, left, col_out, col, folded );
            }
        }
    } else {
        for( int i = size() - 1; i >= 0; --i ) {
            if( line < top ) {
                break;
            }

            const game_message &m = player_messages.messages[i];
            if( message_exceeds_ttl( m ) ) {
                break;
            }

            const nc_color col = m.get_color( player_messages.curmes );
            std::string message_text = m.get_with_count();
            if( !m.is_recent( player_messages.curmes ) ) {
                message_text = remove_color_tags( message_text );
            }

            const auto folded_strings = foldstring( message_text, maxlength );
            const auto folded_rend = folded_strings.rend();
            for( auto string_iter = folded_strings.rbegin();
                 string_iter != folded_rend && line >= top; ++string_iter, line-- ) {
                // Redrawing line to ensure new messages similar to previous
                // messages will not be missed by screen readers
                wredrawln( ipk_target, line, 1 );
                nc_color col_out = col;
                print_colored_text( ipk_target, line, left, col_out, col, *string_iter );
            }
        }
    }

    player_messages.curmes = calendar::turn;
}

void add_msg( std::string msg )
{
    Messages::add_msg( std::move( msg ) );
}

void add_msg( game_message_type const type, std::string msg )
{
    Messages::add_msg( type, std::move( msg ) );
}
