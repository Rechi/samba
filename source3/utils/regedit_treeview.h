/*
 * Samba Unix/Linux SMB client library
 * Registry Editor
 * Copyright (C) Christopher Davis 2012
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _REGEDIT_TREEVIEW_H_
#define _REGEDIT_TREEVIEW_H_

#include "includes.h"
#include <ncurses.h>
#include <menu.h>
#include <panel.h>

struct registry_key;

struct tree_node {

	char *name;
	char *label;
	struct registry_key *key;

	struct tree_node *parent;
	struct tree_node *child_head;
	struct tree_node *previous;
	struct tree_node *next;
};

struct tree_view {

	struct tree_node *root;
	WINDOW *window;
	PANEL *panel;
	MENU *menu;
	ITEM **current_items;
	ITEM *empty[2];
};

struct tree_node *tree_node_new(TALLOC_CTX *ctx, struct tree_node *parent,
				const char *name, struct registry_key *key);
void tree_node_append(struct tree_node *left, struct tree_node *right);
struct tree_node *tree_node_pop(struct tree_node **plist);
struct tree_node *tree_node_first(struct tree_node *list);
struct tree_node *tree_node_last(struct tree_node *list);
void tree_node_append_last(struct tree_node *list, struct tree_node *node);
void tree_node_free_recursive(struct tree_node *list);
size_t tree_node_print_path(WINDOW *label, struct tree_node *node);
struct tree_view *tree_view_new(TALLOC_CTX *ctx, struct tree_node *root,
				int nlines, int ncols,
				int begin_y, int begin_x);
void tree_view_resize(struct tree_view *view, int nlines, int ncols,
			     int begin_y, int begin_x);
void tree_view_show(struct tree_view *view);
void tree_view_clear(struct tree_view *view);
WERROR tree_view_update(struct tree_view *view, struct tree_node *list);
bool tree_node_has_children(struct tree_node *node);
WERROR tree_node_load_children(struct tree_node *node);

#endif
