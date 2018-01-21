/*
    Copyright (C) 2000-2006 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/
#ifdef WAF_BUILD
#include "gtk2ardour-config.h"
#endif

#include <cstdio>
#include <map>

#include <algorithm>

#include <gtkmm/button.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/frame.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/notebook.h>
#include <gtkmm/stock.h>
#include <gtkmm/table.h>

#include "gtkmm2ext/utils.h"

#include "widgets/tooltips.h"

#include "pbd/convert.h"
#include "pbd/tokenizer.h"

#include "ardour/utils.h"

#include "plugin_selector.h"
#include "gui_thread.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace PBD;
using namespace Gtk;
using namespace std;
using namespace ArdourWidgets;

PluginSelector::PluginSelector (PluginManager& mgr)
	: ArdourDialog (_("Plugin Manager"), true, false)
	, search_clear_button (Stock::CLEAR)
	, manager (mgr)
	, inhibit_refill (false)
{
	set_name ("PluginSelectorWindow");
	add_events (Gdk::KEY_PRESS_MASK|Gdk::KEY_RELEASE_MASK);

	_plugin_menu = 0;
	in_row_change = false;

	manager.PluginListChanged.connect (plugin_list_changed_connection, invalidator (*this), boost::bind (&PluginSelector::build_plugin_menu, this), gui_context());
	manager.PluginListChanged.connect (plugin_list_changed_connection, invalidator (*this), boost::bind (&PluginSelector::refill, this), gui_context());
	
	manager.PluginStatusesChanged.connect (plugin_list_changed_connection, invalidator (*this), boost::bind (&PluginSelector::plugin_status_changed, this, _1, _2, _3), gui_context());

	manager.PluginTagsChanged.connect(plugin_list_changed_connection, invalidator (*this), boost::bind (&PluginSelector::tags_changed, this, _1, _2, _3), gui_context());


	plugin_model = Gtk::ListStore::create (plugin_columns);
	plugin_display.set_model (plugin_model);
	/* XXX translators: try to convert "Fav" into a short term
	   related to "favorite" and "Hid" into a short term
	   related to "hidden"
	*/
	plugin_display.append_column (_("Fav"), plugin_columns.favorite);
	plugin_display.append_column (_("Hide"), plugin_columns.hidden);
	plugin_display.append_column (_("Name"), plugin_columns.name);
//	plugin_display.append_column (_("Category"), plugin_columns.category);
	plugin_display.append_column (_("Tags"), plugin_columns.tags);
	plugin_display.append_column (_("Creator"), plugin_columns.creator);
	plugin_display.append_column (_("Type"), plugin_columns.type_name);
	plugin_display.append_column (_("# Audio In"),plugin_columns.audio_ins);
	plugin_display.append_column (_("# Audio Out"), plugin_columns.audio_outs);
	plugin_display.append_column (_("# MIDI In"),plugin_columns.midi_ins);
	plugin_display.append_column (_("# MIDI Out"), plugin_columns.midi_outs);
	plugin_display.set_headers_visible (true);
	plugin_display.set_headers_clickable (true);
	plugin_display.set_reorderable (false);
	plugin_display.set_rules_hint (true);
	plugin_display.add_object_drag (plugin_columns.plugin.index(), "PluginInfoPtr");
	plugin_display.set_drag_column (plugin_columns.name.index());

	// setting a sort-column prevents re-ordering via Drag/Drop
	plugin_model->set_sort_column (plugin_columns.name.index(), Gtk::SORT_ASCENDING);

	CellRendererToggle* fav_cell = dynamic_cast<CellRendererToggle*>(plugin_display.get_column_cell_renderer (0));
	fav_cell->property_activatable() = true;
	fav_cell->property_radio() = true;
	fav_cell->signal_toggled().connect (sigc::mem_fun (*this, &PluginSelector::favorite_changed));

	CellRendererToggle* hidden_cell = dynamic_cast<CellRendererToggle*>(plugin_display.get_column_cell_renderer (1));
	hidden_cell->property_activatable() = true;
	hidden_cell->property_radio() = true;
	hidden_cell->signal_toggled().connect (sigc::mem_fun (*this, &PluginSelector::hidden_changed));

	scroller.set_border_width(10);
	scroller.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	scroller.add(plugin_display);

	amodel = Gtk::ListStore::create(acols);
	added_list.set_model (amodel);
	added_list.append_column (_("Plugins to be connected"), acols.text);
	added_list.set_headers_visible (true);
	added_list.set_reorderable (false);

	for (int i = 0; i <=8; i++) {
		Gtk::TreeView::Column* column = plugin_display.get_column(i);
		column->set_sort_column(i);
	}

	ascroller.set_border_width(10);
	ascroller.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	ascroller.add(added_list);
	btn_add = manage(new Gtk::Button(Stock::ADD));
	set_tooltip(*btn_add, _("Add a plugin to the effect list"));
	btn_add->set_sensitive (false);
	btn_remove = manage(new Gtk::Button(Stock::REMOVE));
	btn_remove->set_sensitive (false);
	set_tooltip(*btn_remove, _("Remove a plugin from the effect list"));

	btn_add->set_name("PluginSelectorButton");
	btn_remove->set_name("PluginSelectorButton");


	Gtk::Table* table = manage(new Gtk::Table(7, 11));
	table->set_size_request(850, 500);

//	filter_mode.signal_changed().connect (sigc::mem_fun (*this, &PluginSelector::filter_mode_changed));

	//------------------- SEARCH

	Gtk::Table* search_table = manage(new Gtk::Table(2, 2));

	search_entry.signal_changed().connect (sigc::mem_fun (*this, &PluginSelector::search_entry_changed));
	search_clear_button.signal_clicked().connect (sigc::mem_fun (*this, &PluginSelector::search_clear_button_clicked));

	_search_name_checkbox = manage (new CheckButton( _("Name") ));
	_search_name_checkbox->set_active();

	_search_tags_checkbox = manage (new CheckButton( _("Tags")));
	_search_tags_checkbox->set_active();

	_search_ignore_checkbox = manage (new CheckButton(_("Ignore Filters when searching")));
	_search_ignore_checkbox->set_active();
	_search_ignore_checkbox->signal_toggled().connect (sigc::mem_fun (*this, &PluginSelector::set_sensitive_widgets));

	Gtk::Label* search_help_label1 = manage (new Label(
		_( "Search terms must \"all\" be matched, to return a hit."), Gtk::ALIGN_LEFT));

	Gtk::Label* search_help_label2 = manage (new Label(
		_( "Ex: \"ess dyn\" will return \"dynamic de-esser\" but not \"de-esser\"." ), Gtk::ALIGN_LEFT));

	search_table->attach (search_entry,            0, 3, 0, 1, FILL|EXPAND, FILL);
	search_table->attach (search_clear_button,     3, 4, 0, 1, FILL, FILL);
	search_table->attach (*_search_name_checkbox,  0, 1, 1, 2, FILL, FILL);
	search_table->attach (*_search_tags_checkbox,  1, 2, 1, 2, FILL, FILL);
	search_table->attach (*_search_ignore_checkbox,2, 3, 1, 2, FILL, FILL);
	search_table->attach (*search_help_label1,      0, 3, 2, 3, FILL, FILL);
	search_table->attach (*search_help_label2,      0, 3, 3, 4, FILL, FILL);

	search_table->set_border_width (4);
	search_table->set_col_spacings (2);
	search_table->set_row_spacings (4);

	Frame* search_frame = manage (new Frame);
	search_frame->set_name ("BaseFrame");
	search_frame->set_label (_("Search"));
	search_frame->add (*search_table);
	search_frame->show_all ();
	
	_search_name_checkbox->signal_clicked().connect (sigc::mem_fun (*this, &PluginSelector::refill));
	_search_tags_checkbox->signal_clicked().connect (sigc::mem_fun (*this, &PluginSelector::refill));

	//------------------- FILTER
	
	Gtk::Table* filter_table = manage(new Gtk::Table(1, 10));

	Gtk::RadioButtonGroup fil_radio_group;

	_fil_effects_radio = manage (new RadioButton(fil_radio_group, _("Show Effects Only")));
	_fil_instruments_radio = manage (new RadioButton(fil_radio_group, _("Show Instruments Only")));
	_fil_utils_radio = manage (new RadioButton(fil_radio_group, _("Show Utilities Only")));
	_fil_favorites_radio = manage (new RadioButton(fil_radio_group, _("Show Favorites Only")));
	_fil_hidden_radio = manage (new RadioButton(fil_radio_group, _("Show Hidden Only")));
	_fil_all_radio = manage (new RadioButton(fil_radio_group, _("Show All")));

	_fil_type_combo = manage ( new ComboBoxText );
	_fil_type_combo->append_text( _("Show All Formats") );
	_fil_type_combo->append_text( X_("VST") );
#ifdef AUDIOUNIT_SUPPORT
	_fil_type_combo->append_text( X_("AudioUnit") );
#endif
	_fil_type_combo->append_text( X_("LV2") );
	_fil_type_combo->append_text( X_("LUA") );
	_fil_type_combo->append_text( X_("LADSPA") );
	_fil_type_combo->set_active_text( _("Show All Formats") );
	
	_fil_creator_combo = manage (new ComboBoxText);
	//note: _fil_creator_combo menu gets filled in build_plugin_menu
	
	_fil_channel_combo = manage (new ComboBoxText);
	_fil_channel_combo->append_text( _("Audio I/O only") );
	_fil_channel_combo->append_text( _("Mono Audio only") );
	_fil_channel_combo->append_text( _("Stereo Audio only") );
	_fil_channel_combo->append_text( _("MIDI I/O only") );
	_fil_channel_combo->append_text( _("Show All I/O") );
	_fil_channel_combo->set_active_text( _("Audio I/O only") );
	

	int p = 0;
	filter_table->attach (*_fil_effects_radio,       2, 3, p, p+1, FILL, FILL); p++;
	filter_table->attach (*_fil_instruments_radio,   2, 3, p, p+1, FILL, FILL); p++;
	filter_table->attach (*_fil_utils_radio,         2, 3, p, p+1, FILL, FILL); p++;
	filter_table->attach (*_fil_favorites_radio,     2, 3, p, p+1, FILL, FILL); p++;
	filter_table->attach (*_fil_hidden_radio,        2, 3, p, p+1, FILL, FILL); p++;
	filter_table->attach (*_fil_all_radio,           2, 3, p, p+1, FILL, FILL); p++;
	filter_table->attach (*_fil_type_combo,             2, 3, p, p+1, FILL, FILL); p++;
	filter_table->attach (*_fil_creator_combo,          2, 3, p, p+1, FILL, FILL); p++;
	filter_table->attach (*_fil_channel_combo,          2, 3, p, p+1, FILL, FILL); p++;

	filter_table->set_border_width (4);
	filter_table->set_col_spacings (4);
	filter_table->set_row_spacings (4);

	Frame* filter_frame = manage (new Frame);
	filter_frame->set_name ("BaseFrame");
	filter_frame->set_label (_("Filter"));
	filter_frame->add (*filter_table);
	filter_frame->show_all ();

	_fil_effects_radio->signal_clicked().connect (sigc::mem_fun (*this, &PluginSelector::refill));
	_fil_instruments_radio->signal_clicked().connect (sigc::mem_fun (*this, &PluginSelector::refill));
	_fil_utils_radio->signal_clicked().connect (sigc::mem_fun (*this, &PluginSelector::refill));
	_fil_favorites_radio->signal_clicked().connect (sigc::mem_fun (*this, &PluginSelector::refill));
	_fil_hidden_radio->signal_clicked().connect (sigc::mem_fun (*this, &PluginSelector::refill));

	_fil_type_combo->signal_changed().connect (sigc::mem_fun (*this, &PluginSelector::refill));
	_fil_creator_combo->signal_changed().connect (sigc::mem_fun (*this, &PluginSelector::refill));
	_fil_channel_combo->signal_changed().connect (sigc::mem_fun (*this, &PluginSelector::refill));

	//--------------------- TAG entry
	
	Gtk::Table* tagging_table = manage(new Gtk::Table(1, 2));
	tagging_table->set_border_width (4);
	tagging_table->set_col_spacings (4);
	tagging_table->set_row_spacings (4);

	tag_entry = manage (new Gtk::Entry);
	tag_entry->signal_changed().connect (sigc::mem_fun (*this, &PluginSelector::tag_entry_changed));

	Gtk::Button* tag_reset_button = manage ( new Button( _("Reset") ));
	tag_reset_button->signal_clicked().connect (sigc::mem_fun (*this, &PluginSelector::tag_reset_button_clicked));

	Gtk::Label* tagging_help_label1 = manage (new Label(
		_( "Enter space-separated, one-word Tags for selected plugin."), Gtk::ALIGN_LEFT));

	Gtk::Label* tagging_help_label2 = manage (new Label(
		_( "You can use dashes or underscores."), Gtk::ALIGN_LEFT));

	Gtk::Label* tagging_help_label3 = manage (new Label(
		_("Ex: \"dynamic de-esser vocal\" applies 3 Tags." ), Gtk::ALIGN_LEFT));

	p = 0;
	tagging_table->attach (*tag_entry,           0, 1, p, p+1, FILL|EXPAND, FILL);
	tagging_table->attach (*tag_reset_button,    1, 2, p, p+1, FILL, FILL); p++;
	tagging_table->attach (*tagging_help_label1, 0, 2, p, p+1, FILL, FILL); p++;
	tagging_table->attach (*tagging_help_label2, 0, 2, p, p+1, FILL, FILL); p++;
	tagging_table->attach (*tagging_help_label3, 0, 2, p, p+1, FILL, FILL); p++;

	Frame* tag_frame = manage (new Frame);
	tag_frame->set_name ("BaseFrame");
	tag_frame->set_label (_("Tags for Selected Plugin"));
	tag_frame->add (*tagging_table);
	tag_frame->show_all ();

//-----------------------Top-level Layout
	

	HBox* side_by_side = manage (new HBox);
	VBox* to_be_inserted_vbox = manage (new VBox);

	table->attach (scroller,               0, 3, 0, 5); //this is the main plugin list
	table->attach (*search_frame,          0, 1, 6, 7, FILL, FILL, 5, 5);
	table->attach (*tag_frame,             0, 1, 7, 8, FILL, FILL, 5, 5);
	table->attach (*filter_frame,          1, 2, 6, 8, FILL, FILL, 5, 5);
	table->attach (*to_be_inserted_vbox,   2, 3, 6, 8, FILL|EXPAND, FILL, 5, 5);  //to be inserted...

	to_be_inserted_vbox->pack_start (ascroller);

	HBox* add_remove = manage (new HBox);
	add_remove->pack_start (*btn_add, true, true);
	add_remove->pack_start (*btn_remove, true, true);

	to_be_inserted_vbox->pack_start (*add_remove, false, false);
	to_be_inserted_vbox->set_size_request (200, -1);

	side_by_side->pack_start (*table);

	add_button (Stock::CLOSE, RESPONSE_CLOSE);
	add_button (_("Insert Plugin(s)"), RESPONSE_APPLY);
	set_default_response (RESPONSE_APPLY);
	set_response_sensitive (RESPONSE_APPLY, false);
	get_vbox()->pack_start (*side_by_side);

	table->set_name("PluginSelectorTable");
	plugin_display.set_name("PluginSelectorDisplay");
	//plugin_display.set_name("PluginSelectorList");
	added_list.set_name("PluginSelectorList");

	plugin_display.signal_row_activated().connect_notify (sigc::mem_fun(*this, &PluginSelector::row_activated));
	plugin_display.get_selection()->signal_changed().connect (sigc::mem_fun(*this, &PluginSelector::display_selection_changed));
	plugin_display.grab_focus();

	btn_add->signal_clicked().connect(sigc::mem_fun(*this, &PluginSelector::btn_add_clicked));
	btn_remove->signal_clicked().connect(sigc::mem_fun(*this, &PluginSelector::btn_remove_clicked));
	added_list.get_selection()->signal_changed().connect (sigc::mem_fun(*this, &PluginSelector::added_list_selection_changed));
	added_list.signal_button_press_event().connect_notify (mem_fun(*this, &PluginSelector::added_row_clicked));

	build_plugin_menu ();
}

PluginSelector::~PluginSelector ()
{
	delete _plugin_menu;
}

void
PluginSelector::row_activated(Gtk::TreeModel::Path, Gtk::TreeViewColumn*)
{
	btn_add_clicked();
}

void
PluginSelector::added_row_clicked(GdkEventButton* event)
{
	if (event->type == GDK_2BUTTON_PRESS)
		btn_remove_clicked();
}

bool
PluginSelector::show_this_plugin (const PluginInfoPtr& info, const std::string& searchstr)
{
	string mode;
	bool maybe_show = false;
	
	if (!searchstr.empty()) {

		std::string compstr;

		if ( _search_name_checkbox->get_active() ) {  //name contains
			compstr = info->name;
			transform (compstr.begin(), compstr.end(), compstr.begin(), ::toupper);
			if (compstr.find (searchstr) != string::npos) {
				maybe_show = true;
			}
		}
		
		if ( _search_tags_checkbox->get_active() ) {  //tag contains
			compstr = manager.get_tags (info);
			transform (compstr.begin(), compstr.end(), compstr.begin(), ::toupper);
			if (compstr.find (searchstr) != string::npos) {
				maybe_show = true;
			}
		}

		if (!maybe_show) {
			return false;
		}
		
		//user asked to ignore filters
		if (maybe_show && _search_ignore_checkbox->get_active()) {
			return true;
		}
	}

	if ( _fil_effects_radio->get_active() && !info->is_effect() ) {
		return false;
	}

	if ( _fil_instruments_radio->get_active() && !info->is_instrument() ) {
		return false;
	}

	if ( _fil_utils_radio->get_active() && !( info->is_utility() || info->is_analyzer()) ) {
		return false;
	}

//	if ( _fil_analyser_radio->get_active() && !info->is_analyzer() ) { //ToDo: separate radio button for analyzers?
//		return false;
//	}


	if ( _fil_favorites_radio->get_active() && !(manager.get_status (info) == PluginManager::Favorite) ) {
		return false;
	}

	if ( _fil_hidden_radio->get_active() && !(manager.get_status (info) == PluginManager::Hidden) ) {
		return false;
	}

	if (manager.get_status (info) == PluginManager::Hidden) {
		if ( !_fil_hidden_radio->get_active() && !_fil_all_radio->get_active() ) {
			return false;
		}
	}

//================== Filter "type" combobox

	if ( _fil_type_combo->get_active_text() == X_("VST") && PluginManager::to_generic_vst(info->type) != LXVST ) {
		return false;
	}
	
	if ( _fil_type_combo->get_active_text() == X_("AudioUnit") && info->type != AudioUnit  ) {
		return false;
	}
	
	if ( _fil_type_combo->get_active_text() == X_("LV2") && info->type !=  LV2 ) {
		return false;
	}
	
	if ( _fil_type_combo->get_active_text() == X_("LUA") && info->type !=  Lua ) {
		return false;
	}
	
	if ( _fil_type_combo->get_active_text() == X_("LADSPA") && info->type != LADSPA  ) {
		return false;
	}

//================== Filter "creator" combobox

	if ( _fil_creator_combo->get_active_text() != _("Show All Creators") ) {
		if ( _fil_creator_combo->get_active_text() != info->creator ) {
			return false;
		}
	}

//================== Filter "I/O" combobox

	if ( _fil_channel_combo->get_active_text() != _("Show All I/O") || info->reconfigurable_io () ) {

//		if ( info->reconfigurable_io () ) {
//			return true;  //who knows.... ?
//		}

		if ( _fil_channel_combo->get_active_text() == _("Audio I/O only") ) {
			if ( info->n_inputs.n_audio() == 0 ||
			   info->n_outputs.n_audio()  == 0 ||
			   info->n_outputs.n_midi()   != 0 ||
			   info->n_outputs.n_midi()   != 0
			   ) {
				return false;
			}	
		}
	
		if ( _fil_channel_combo->get_active_text() == _("Mono Audio only") ) {
			if ( info->n_inputs.n_audio() != 1 ||
			   info->n_outputs.n_audio()  != 1 ||
			   info->n_outputs.n_midi()   != 0 ||
			   info->n_outputs.n_midi()   != 0
			   ) {
				return false;
			}	
		}
	
		if ( _fil_channel_combo->get_active_text() == _("Stereo Audio only") ) {
			if ( info->n_inputs.n_audio() != 2 ||
			   info->n_outputs.n_audio()  != 2 ||
			   info->n_outputs.n_midi()   != 0 ||
			   info->n_outputs.n_midi()   != 0
			   ) {
				return false;
			}	
		}
	
		if ( _fil_channel_combo->get_active_text() == _("MIDI I/O only") ) {
			if ( info->n_inputs.n_audio()   != 0 ||
			     info->n_outputs.n_audio()  != 0 ||
			     (info->n_inputs.n_midi()  	== 0 && info->n_outputs.n_midi() == 0)
			 	) {
				return false;
			}	
		}
	
	}

	return true;
}

void
PluginSelector::setup_search_string (string& searchstr)
{
	searchstr = search_entry.get_text ();
	transform (searchstr.begin(), searchstr.end(), searchstr.begin(), ::toupper);
}

void
PluginSelector::set_sensitive_widgets ()
{
	if (_search_ignore_checkbox->get_active() && (search_entry.get_text() != "") ) {
		_fil_effects_radio->set_sensitive(false);
		_fil_instruments_radio->set_sensitive(false);
		_fil_utils_radio->set_sensitive(false);
		_fil_favorites_radio->set_sensitive(false);
		_fil_hidden_radio->set_sensitive(false);
		_fil_all_radio->set_sensitive(false);
		_fil_type_combo->set_sensitive(false);
		_fil_creator_combo->set_sensitive(false);
		_fil_channel_combo->set_sensitive(false);
	} else {
		_fil_effects_radio->set_sensitive(true);
		_fil_instruments_radio->set_sensitive(true);
		_fil_utils_radio->set_sensitive(true);
		_fil_favorites_radio->set_sensitive(true);
		_fil_hidden_radio->set_sensitive(true);
		_fil_all_radio->set_sensitive(true);
		_fil_type_combo->set_sensitive(true);
		_fil_creator_combo->set_sensitive(true);
		_fil_channel_combo->set_sensitive(true);
	}
}

void
PluginSelector::refill ()
{
	if (inhibit_refill) {
		return;
	}
	
	std::string searchstr;

	in_row_change = true;

	plugin_model->clear ();

	setup_search_string (searchstr);

	ladspa_refiller (searchstr);
	lv2_refiller (searchstr);
	vst_refiller (searchstr);
	lxvst_refiller (searchstr);
	mac_vst_refiller (searchstr);
	au_refiller (searchstr);
	lua_refiller (searchstr);

	in_row_change = false;
}

void
PluginSelector::refiller (const PluginInfoList& plugs, const::std::string& searchstr, const char* type)
{
	char buf[16];

	for (PluginInfoList::const_iterator i = plugs.begin(); i != plugs.end(); ++i) {

		if (show_this_plugin (*i, searchstr)) {

			TreeModel::Row newrow = *(plugin_model->append());
			newrow[plugin_columns.favorite] = (manager.get_status (*i) == PluginManager::Favorite);
			newrow[plugin_columns.hidden] = (manager.get_status (*i) == PluginManager::Hidden);
			newrow[plugin_columns.name] = (*i)->name;
			newrow[plugin_columns.type_name] = type;
			newrow[plugin_columns.category] = (*i)->category;

			string creator = (*i)->creator;
			string::size_type pos = 0;

			if ((*i)->type == ARDOUR::LADSPA) {
				/* stupid LADSPA creator strings */
#ifdef PLATFORM_WINDOWS
				while (pos < creator.length() && creator[pos] > -2 && creator[pos] < 256 && (isalnum (creator[pos]) || isspace (creator[pos]))) ++pos;
#else
				while (pos < creator.length() && (isalnum (creator[pos]) || isspace (creator[pos]))) ++pos;
#endif
			} else {
				pos = creator.length ();
			}
			// If there were too few characters to create a
			// meaningful name, mark this creator as 'Unknown'
			if (creator.length() < 2 || pos < 3) {
				creator = "Unknown";
			} else{
				creator = creator.substr (0, pos);
			}

			newrow[plugin_columns.creator] = creator;

			newrow[plugin_columns.tags] = manager.get_tags(*i);

			if ((*i)->reconfigurable_io ()) {
				newrow[plugin_columns.audio_ins] = _("variable");
				newrow[plugin_columns.midi_ins] = _("variable");
				newrow[plugin_columns.audio_outs] = _("variable");
				newrow[plugin_columns.midi_outs] = _("variable");
			} else {
				snprintf (buf, sizeof(buf), "%d", (*i)->n_inputs.n_audio());
				newrow[plugin_columns.audio_ins] = buf;
				snprintf (buf, sizeof(buf), "%d", (*i)->n_inputs.n_midi());
				newrow[plugin_columns.midi_ins] = buf;

				snprintf (buf, sizeof(buf), "%d", (*i)->n_outputs.n_audio());
				newrow[plugin_columns.audio_outs] = buf;
				snprintf (buf, sizeof(buf), "%d", (*i)->n_outputs.n_midi());
				newrow[plugin_columns.midi_outs] = buf;
			}

			newrow[plugin_columns.plugin] = *i;
		}
	}
}

void
PluginSelector::ladspa_refiller (const std::string& searchstr)
{
	refiller (manager.ladspa_plugin_info(), searchstr, "LADSPA");
}

void
PluginSelector::lua_refiller (const std::string& searchstr)
{
	refiller (manager.lua_plugin_info(), searchstr, "Lua");
}

void
PluginSelector::lv2_refiller (const std::string& searchstr)
{
#ifdef LV2_SUPPORT
	refiller (manager.lv2_plugin_info(), searchstr, "LV2");
#endif
}

void
#ifdef WINDOWS_VST_SUPPORT
PluginSelector::vst_refiller (const std::string& searchstr)
#else
PluginSelector::vst_refiller (const std::string&)
#endif
{
#ifdef WINDOWS_VST_SUPPORT
	refiller (manager.windows_vst_plugin_info(), searchstr, "VST");
#endif
}

void
#ifdef LXVST_SUPPORT
PluginSelector::lxvst_refiller (const std::string& searchstr)
#else
PluginSelector::lxvst_refiller (const std::string&)
#endif
{
#ifdef LXVST_SUPPORT
	refiller (manager.lxvst_plugin_info(), searchstr, "LXVST");
#endif
}

void
#ifdef MACVST_SUPPORT
PluginSelector::mac_vst_refiller (const std::string& searchstr)
#else
PluginSelector::mac_vst_refiller (const std::string&)
#endif
{
#ifdef MACVST_SUPPORT
	refiller (manager.mac_vst_plugin_info(), searchstr, "MacVST");
#endif
}

void
#ifdef AUDIOUNIT_SUPPORT
PluginSelector::au_refiller (const std::string& searchstr)
#else
PluginSelector::au_refiller (const std::string&)
#endif
{
#ifdef AUDIOUNIT_SUPPORT
	refiller (manager.au_plugin_info(), searchstr, "AU");
#endif
}

PluginPtr
PluginSelector::load_plugin (PluginInfoPtr pi)
{
	if (_session == 0) {
		return PluginPtr();
	}

	return pi->load (*_session);
}

void
PluginSelector::btn_add_clicked()
{
	std::string name;
	PluginInfoPtr pi;
	TreeModel::Row newrow = *(amodel->append());
	TreeModel::Row row;

	row = *(plugin_display.get_selection()->get_selected());
	name = row[plugin_columns.name];
	pi = row[plugin_columns.plugin];

	newrow[acols.text] = name;
	newrow[acols.plugin] = pi;

	if (!amodel->children().empty()) {
		set_response_sensitive (RESPONSE_APPLY, true);
	}
}

void
PluginSelector::btn_remove_clicked()
{
	TreeModel::iterator iter = added_list.get_selection()->get_selected();

	amodel->erase(iter);
	if (amodel->children().empty()) {
		set_response_sensitive (RESPONSE_APPLY, false);
	}
}

void
PluginSelector::display_selection_changed()
{
	if (plugin_display.get_selection()->count_selected_rows() != 0) {
		btn_add->set_sensitive (true);

		//a plugin row is selected; allow the user to edit the "tags" on it.
		TreeModel::Row row = *(plugin_display.get_selection()->get_selected());
		std::string tags = row[plugin_columns.tags];
		tag_entry->set_text( tags );

	} else {
		btn_add->set_sensitive (false);
		tag_entry->set_text( "" );
	}
}

void
PluginSelector::added_list_selection_changed()
{
	if (added_list.get_selection()->count_selected_rows() != 0) {
		btn_remove->set_sensitive (true);
	} else {
		btn_remove->set_sensitive (false);
	}
}

int
PluginSelector::run ()
{
	ResponseType r;
	TreeModel::Children::iterator i;

	bool finish = false;

	while (!finish) {

		SelectedPlugins plugins;
		r = (ResponseType) Dialog::run ();

		switch (r) {
		case RESPONSE_APPLY:
			for (i = amodel->children().begin(); i != amodel->children().end(); ++i) {
				PluginInfoPtr pp = (*i)[acols.plugin];
				PluginPtr p = load_plugin (pp);
				if (p) {
					plugins.push_back (p);
				} else {
					MessageDialog msg (string_compose (_("The plugin \"%1\" could not be loaded\n\nSee the Log window for more details (maybe)"), pp->name));
					msg.run ();
				}
			}
			if (interested_object && !plugins.empty()) {
				finish = !interested_object->use_plugins (plugins);
			}

			break;

		default:
			finish = true;
			break;
		}
	}


	hide();
	amodel->clear();
	interested_object = 0;
	
	if ( _need_tag_save ) {
		manager.save_tags();
	}

	if ( _need_status_save ) {
		manager.save_statuses();
	}

	if ( _need_menu_rebuild ) {
		build_plugin_menu();
	}

	return (int) r;
}

void
PluginSelector::search_clear_button_clicked ()
{
	search_entry.set_text ("");
}

void
PluginSelector::tag_reset_button_clicked ()
{
	if (plugin_display.get_selection()->count_selected_rows() != 0) {
		TreeModel::Row row = *(plugin_display.get_selection()->get_selected());
		std::string str = row[plugin_columns.category];
		std::transform (str.begin(), str.end(), str.begin(), ::tolower);
		tag_entry->set_text ( str );
	}	
}

void
PluginSelector::search_entry_changed ()
{
	set_sensitive_widgets();
	refill ();
}

void
PluginSelector::tag_entry_changed ()
{
	if (plugin_display.get_selection()->count_selected_rows() != 0) {
		TreeModel::Row row = *(plugin_display.get_selection()->get_selected());

		ARDOUR::PluginInfoPtr pi = row[plugin_columns.plugin];
		manager.set_tags( pi->type, pi->unique_id, tag_entry->get_text() );

		_need_tag_save = true;
	}
}

void
PluginSelector::tags_changed (PluginType t, std::string unique_id, std::string tag)
{
	if (plugin_display.get_selection()->count_selected_rows() != 0) {
		TreeModel::Row row = *(plugin_display.get_selection()->get_selected());
		row[plugin_columns.tags] = tag;
	}
	
	//a plugin's tags change while the user is entering them.
	//defer a rebuilding of the "tag" menu until the dialog is closed.
	_need_menu_rebuild = true;
}

void
PluginSelector::plugin_status_changed (PluginType t, std::string uid, PluginManager::PluginStatusType stat)
{
	Gtk::TreeModel::iterator i;
	for (i = plugin_model->children().begin(); i != plugin_model->children().end(); ++i) {
		PluginInfoPtr pp = (*i)[plugin_columns.plugin];	
		if ( (pp->type == t) && (pp->unique_id == uid) ) {
			(*i)[plugin_columns.favorite] = ( stat==PluginManager::Favorite ) ? true : false;
			(*i)[plugin_columns.hidden] = ( stat==PluginManager::Hidden ) ? true : false;

			//if plug was hidden, remove it from the view
			if (stat==PluginManager::Hidden) {
				plugin_model->erase(i);
			}

			//plugin menu must be re-built to accommodate Hidden and Favorite plugins
			build_plugin_menu();

			return;
		}
	}
}

void
PluginSelector::on_show ()
{
	ArdourDialog::on_show ();
	search_entry.grab_focus ();

	refill ();
	
	_need_tag_save = false;
	_need_status_save = false;
	_need_menu_rebuild = false;
}

struct PluginMenuCompareByCreator {
	bool operator() (PluginInfoPtr a, PluginInfoPtr b) const {
		int cmp;

		cmp = cmp_nocase_utf8 (a->creator, b->creator);

		if (cmp < 0) {
			return true;
		} else if (cmp == 0) {
			/* same creator ... compare names */
			if (cmp_nocase_utf8 (a->name, b->name) < 0) {
				return true;
			}
		}
		return false;
	}
};

struct PluginMenuCompareByName {
	bool operator() (PluginInfoPtr a, PluginInfoPtr b) const {
		int cmp;

		cmp = cmp_nocase_utf8 (a->name, b->name);

		if (cmp < 0) {
			return true;
		} else if (cmp == 0) {
			/* same name ... compare type */
			if (a->type < b->type) {
				return true;
			}
		}
		return false;
	}
};

struct PluginMenuCompareByCategory {
	bool operator() (PluginInfoPtr a, PluginInfoPtr b) const {
		int cmp;

		cmp = cmp_nocase_utf8 (a->category, b->category);

		if (cmp < 0) {
			return true;
		} else if (cmp == 0) {
			/* same category ... compare names */
			if (cmp_nocase_utf8 (a->name, b->name) < 0) {
				return true;
			}
		}
		return false;
	}
};

/** @return Plugin menu. The caller should not delete it */
Gtk::Menu*
PluginSelector::plugin_menu()
{
	return _plugin_menu;
}

void
PluginSelector::build_plugin_menu ()
{
	PluginInfoList all_plugs;

	all_plugs.insert (all_plugs.end(), manager.ladspa_plugin_info().begin(), manager.ladspa_plugin_info().end());
	all_plugs.insert (all_plugs.end(), manager.lua_plugin_info().begin(), manager.lua_plugin_info().end());
#ifdef WINDOWS_VST_SUPPORT
	all_plugs.insert (all_plugs.end(), manager.windows_vst_plugin_info().begin(), manager.windows_vst_plugin_info().end());
#endif
#ifdef LXVST_SUPPORT
	all_plugs.insert (all_plugs.end(), manager.lxvst_plugin_info().begin(), manager.lxvst_plugin_info().end());
#endif
#ifdef MACVST_SUPPORT
	all_plugs.insert (all_plugs.end(), manager.mac_vst_plugin_info().begin(), manager.mac_vst_plugin_info().end());
#endif
#ifdef AUDIOUNIT_SUPPORT
	all_plugs.insert (all_plugs.end(), manager.au_plugin_info().begin(), manager.au_plugin_info().end());
#endif
#ifdef LV2_SUPPORT
	all_plugs.insert (all_plugs.end(), manager.lv2_plugin_info().begin(), manager.lv2_plugin_info().end());
#endif

	using namespace Menu_Helpers;

	delete _plugin_menu;

	_plugin_menu = manage (new Menu);
	_plugin_menu->set_name("ArdourContextMenu");

	MenuList& items = _plugin_menu->items();
	items.clear ();

	Gtk::Menu* favs = create_favs_menu(all_plugs);
	items.push_back (MenuElem (_("Favorites"), *manage (favs)));

	items.push_back (MenuElem (_("Plugin Manager..."), sigc::mem_fun (*this, &PluginSelector::show_manager)));
	items.push_back (SeparatorElem ());

	Menu* by_creator = create_by_creator_menu(all_plugs);
	items.push_back (MenuElem (_("By Creator"), *manage (by_creator)));

	Menu* by_tags = create_by_tags_menu(all_plugs);
	items.push_back (MenuElem (_("By Tags"), *manage (by_tags)));
}

string
GetPluginTypeStr(PluginInfoPtr info)
{
	string type;
	
	switch (info->type) {
	case LADSPA:
		type = X_(" (LADSPA)");
		break;
	case AudioUnit:
		type = X_(" (AU)");
		break;
	case LV2:
		type = X_(" (LV2)");
		break;
	case Windows_VST:
	case LXVST:
	case MacVST:
		type = X_(" (VST)");
		break;
	case Lua:
		type = X_(" (Lua)");
		break;
	}
	
	return type;
}

Gtk::Menu*
PluginSelector::create_favs_menu (PluginInfoList& all_plugs)
{
	using namespace Menu_Helpers;

	Menu* favs = new Menu();
	favs->set_name("ArdourContextMenu");

	PluginMenuCompareByName cmp_by_name;
	all_plugs.sort (cmp_by_name);

	for (PluginInfoList::const_iterator i = all_plugs.begin(); i != all_plugs.end(); ++i) {
		if (manager.get_status (*i) == PluginManager::Favorite) {
			string typ = GetPluginTypeStr(*i);
			MenuElem elem ((*i)->name + typ, (sigc::bind (sigc::mem_fun (*this, &PluginSelector::plugin_chosen_from_menu), *i)));
			elem.get_child()->set_use_underline (false);
			favs->items().push_back (elem);
		}
	}
	return favs;
}

Gtk::Menu*
PluginSelector::create_by_creator_menu (ARDOUR::PluginInfoList& all_plugs)
{
	inhibit_refill = true;
	_fil_creator_combo->clear();
	_fil_creator_combo->append_text( _("Show All Creators") );
	_fil_creator_combo->set_active_text( _("Show All Creators") );
	inhibit_refill = false;

	using namespace Menu_Helpers;

	typedef std::map<std::string,Gtk::Menu*> SubmenuMap;
	SubmenuMap creator_submenu_map;

	Menu* by_creator = new Menu();
	by_creator->set_name("ArdourContextMenu");

	MenuList& by_creator_items = by_creator->items();
	PluginMenuCompareByCreator cmp_by_creator;
	all_plugs.sort (cmp_by_creator);

	for (PluginInfoList::const_iterator i = all_plugs.begin(); i != all_plugs.end(); ++i) {

		if (manager.get_status (*i) == PluginManager::Hidden) continue;

		string creator = (*i)->creator;
		string::size_type pos = 0;

		if ((*i)->type == ARDOUR::LADSPA) {
			/* stupid LADSPA creator strings */
#ifdef PLATFORM_WINDOWS
			while (pos < creator.length() && creator[pos] > -2 && creator[pos] < 256 && (isalnum (creator[pos]) || isspace (creator[pos]))) ++pos;
#else
			while (pos < creator.length() && (isalnum (creator[pos]) || isspace (creator[pos]))) ++pos;
#endif
		} else {
			pos = creator.length ();
		}

		// If there were too few characters to create a
		// meaningful name, mark this creator as 'Unknown'
		if (creator.length() < 2 || pos < 3) {
			creator = "Unknown";
		} else{
			creator = creator.substr (0, pos);
		}

		SubmenuMap::iterator x;
		Gtk::Menu* submenu;
		if ((x = creator_submenu_map.find (creator)) != creator_submenu_map.end()) {
			submenu = x->second;
		} else {
			
			_fil_creator_combo->append_text(creator);
			
			submenu = new Gtk::Menu;
			by_creator_items.push_back (MenuElem (creator, *manage (submenu)));
			creator_submenu_map.insert (pair<std::string,Menu*> (creator, submenu));
			submenu->set_name("ArdourContextMenu");
		}
		string typ = GetPluginTypeStr(*i);
		MenuElem elem ((*i)->name+typ, (sigc::bind (sigc::mem_fun (*this, &PluginSelector::plugin_chosen_from_menu), *i)));
		elem.get_child()->set_use_underline (false);
		submenu->items().push_back (elem);
	}
	
	return by_creator;
}

Gtk::Menu*
PluginSelector::create_by_tags_menu (ARDOUR::PluginInfoList& all_plugs)
{
	using namespace Menu_Helpers;

	typedef std::map<std::string,Gtk::Menu*> SubmenuMap;
	SubmenuMap tags_submenu_map;

	Menu* by_tags = new Menu();
	by_tags->set_name("ArdourContextMenu");
	MenuList& by_tags_items = by_tags->items();

	std::vector<std::string> all_tags = manager.get_all_tags(false);
	for (vector<string>::iterator t = all_tags.begin(); t != all_tags.end(); ++t) {
		Gtk::Menu *submenu = new Gtk::Menu;
		by_tags_items.push_back (MenuElem (*t, *manage (submenu)));
		tags_submenu_map.insert (pair<std::string,Menu*> (*t, submenu));
		submenu->set_name("ArdourContextMenu");
	}
	
	for (PluginInfoList::const_iterator i = all_plugs.begin(); i != all_plugs.end(); ++i) {

		if (manager.get_status (*i) == PluginManager::Hidden) continue;

		//for each tag in the plugins tag list, add it to that submenu
		string tags = manager.get_tags(*i);
		vector<string> tokens;
		if (!PBD::tokenize ( tags, string(",\n"), std::back_inserter (tokens), true)) {
			warning << _("PluginManager: Could not tokenize string: ") << tags << endmsg;
			continue;
		}
		for (vector<string>::iterator t = tokens.begin(); t != tokens.end(); ++t) {
			SubmenuMap::iterator x;
			Gtk::Menu* submenu;
			if ((x = tags_submenu_map.find (*t)) != tags_submenu_map.end()) {
				submenu = x->second;
			} else {
			}
			string typ = GetPluginTypeStr(*i);
			MenuElem elem ((*i)->name + typ, (sigc::bind (sigc::mem_fun (*this, &PluginSelector::plugin_chosen_from_menu), *i)));
			elem.get_child()->set_use_underline (false);
			submenu->items().push_back (elem);
		}
	}
	return by_tags;
}

void
PluginSelector::plugin_chosen_from_menu (const PluginInfoPtr& pi)
{
	PluginPtr p = load_plugin (pi);

	if (p && interested_object) {
		SelectedPlugins plugins;
		plugins.push_back (p);
		interested_object->use_plugins (plugins);
	}

	interested_object = 0;
}

void
PluginSelector::favorite_changed (const std::string& path)
{
	PluginInfoPtr pi;

	if (in_row_change) {
		return;
	}

	in_row_change = true;

	TreeModel::iterator iter = plugin_model->get_iter (path);

	if (iter) {

		bool favorite = !(*iter)[plugin_columns.favorite];

		/* change state */

		PluginManager::PluginStatusType status = (favorite ? PluginManager::Favorite : PluginManager::Normal);

		/* save new statuses list */

		pi = (*iter)[plugin_columns.plugin];

		manager.set_status (pi->type, pi->unique_id, status);

		_need_status_save = true;
	}
	in_row_change = false;
}

void
PluginSelector::hidden_changed (const std::string& path)
{
	PluginInfoPtr pi;

	if (in_row_change) {
		return;
	}

	in_row_change = true;

	TreeModel::iterator iter = plugin_model->get_iter (path);

	if (iter) {

		bool hidden = !(*iter)[plugin_columns.hidden];

		/* change state */

		PluginManager::PluginStatusType status = (hidden ? PluginManager::Hidden : PluginManager::Normal);

		/* save new statuses list */

		pi = (*iter)[plugin_columns.plugin];

		manager.set_status (pi->type, pi->unique_id, status);

		_need_status_save = true;
	}
	in_row_change = false;
}

void
PluginSelector::show_manager ()
{
	show_all();
	run ();
}

void
PluginSelector::set_interested_object (PluginInterestedObject& obj)
{
	interested_object = &obj;
}
