#include <stdlib.h>
#include <stdbool.h>
#include <strings.h>
#include "config.h"
#include "container.h"
#include "workspace.h"
#include "focus.h"
#include "layout.h"
#include "log.h"


static swayc_t *new_swayc(enum swayc_types type) {
	swayc_t *c = calloc(1, sizeof(swayc_t));
	c->handle = -1;
	c->layout = L_NONE;
	c->type = type;
	c->weight  = 1;
	if (type != C_VIEW) {
		c->children = create_list();
	}
	return c;
}

static void free_swayc(swayc_t *c) {
	// TODO does not properly handle containers with children,
	// TODO but functions that call this usually check for that
	if (c->children) {
		if (c->children->length) {
			int i;
			for (i = 0; i < c->children->length; ++i) {
				free_swayc(c->children->items[i]);
			}
		}
		list_free(c->children);
	}
	if (c->floating) {
		if (c->floating->length) {
			int i;
			for (i = 0; i < c->floating->length; ++i) {
				free_swayc(c->floating->items[i]);
			}
		}
		list_free(c->floating);
	}
	if (c->parent) {
		remove_child(c);
	}
	if (c->name) {
		free(c->name);
	}
	free(c);
}

/* New containers */

static bool workspace_test(swayc_t *view, void *name) {
	return strcasecmp(view->name, (char *)name);
}

swayc_t *new_output(wlc_handle handle) {
	const struct wlc_size* size = wlc_output_get_resolution(handle);
	const char *name = wlc_output_get_name(handle);
	sway_log(L_DEBUG, "Added output %lu:%s", handle, name);

	swayc_t *output = new_swayc(C_OUTPUT);
	output->width = size->w;
	output->height = size->h;
	output->handle = handle;
	output->name = name ? strdup(name) : NULL;
	output->gaps = config->gaps_outer;

	add_child(&root_container, output);

	// Create workspace
	char *ws_name = NULL;
	if (name) {
		int i;
		for (i = 0; i < config->workspace_outputs->length; ++i) {
			struct workspace_output *wso = config->workspace_outputs->items[i];
			if (strcasecmp(wso->output, name) == 0) {
				sway_log(L_DEBUG, "Matched workspace to output: %s for %s", wso->workspace, wso->output);
				// Check if any other workspaces are using this name
				if (find_container(&root_container, workspace_test, wso->workspace)) {
					break;
				}
				ws_name = strdup(wso->workspace);
				break;
			}
		}
	}
	if (!ws_name) {
		ws_name = workspace_next_name();
	}

	// create and initilize default workspace
	swayc_t *ws = new_workspace(output, ws_name);
	ws->is_focused = true;

	free(ws_name);
	
	return output;
}

swayc_t *new_workspace(swayc_t *output, const char *name) {
	sway_log(L_DEBUG, "Added workspace %s for output %u", name, (unsigned int)output->handle);
	swayc_t *workspace = new_swayc(C_WORKSPACE);

	workspace->layout = L_HORIZ; // TODO: default layout
	workspace->x = output->x;
	workspace->y = output->y;
	workspace->width = output->width;
	workspace->height = output->height;
	workspace->name = strdup(name);
	workspace->visible = true;
	workspace->floating = create_list();

	add_child(output, workspace);
	return workspace;
}

swayc_t *new_container(swayc_t *child, enum swayc_layouts layout) {
	swayc_t *cont = new_swayc(C_CONTAINER);

	sway_log(L_DEBUG, "creating container %p around %p", cont, child);

	cont->layout = layout;
	cont->width = child->width;
	cont->height = child->height;
	cont->x = child->x;
	cont->y = child->y;
	cont->visible = child->visible;

	/* Container inherits all of workspaces children, layout and whatnot */
	if (child->type == C_WORKSPACE) {
		swayc_t *workspace = child;
		// reorder focus
		cont->focused = workspace->focused;
		workspace->focused = cont;
		// set all children focu to container
		int i;
		for (i = 0; i < workspace->children->length; ++i) {
			((swayc_t *)workspace->children->items[i])->parent = cont;
		}
		// Swap children
		list_t  *tmp_list  = workspace->children;
		workspace->children = cont->children;
		cont->children = tmp_list;
		// add container to workspace chidren
		add_child(workspace, cont);
		// give them proper layouts
		cont->layout = workspace->layout;
		workspace->layout = layout;
	} else { // Or is built around container
		swayc_t *parent = replace_child(child, cont);
		if (parent) {
			add_child(cont, child);
		}
	}
	return cont;
}

swayc_t *new_view(swayc_t *sibling, wlc_handle handle) {
	const char *title = wlc_view_get_title(handle);
	swayc_t *view = new_swayc(C_VIEW);
	sway_log(L_DEBUG, "Adding new view %lu:%s to container %p %d",
		handle, title, sibling, sibling ? sibling->type : 0);
	// Setup values
	view->handle = handle;
	view->name = title ? strdup(title) : NULL;
	view->visible = true;
	view->is_focused = true;

	view->gaps = config->gaps_inner;

	view->desired_width = -1;
	view->desired_height = -1;

	view->is_floating = false;

	if (sibling->type == C_WORKSPACE) {
		// Case of focused workspace, just create as child of it
		add_child(sibling, view);
	} else {
		// Regular case, create as sibling of current container
		add_sibling(sibling, view);
	}
	return view;
}

swayc_t *new_floating_view(wlc_handle handle) {
	const char *title = wlc_view_get_title(handle);
	swayc_t *view = new_swayc(C_VIEW);
	sway_log(L_DEBUG, "Adding new view %lu:%x:%s as a floating view",
		handle, wlc_view_get_type(handle), title);
	// Setup values
	view->handle = handle;
	view->name = title ? strdup(title) : NULL;
	view->visible = true;

	// Set the geometry of the floating view
	const struct wlc_geometry* geometry = wlc_view_get_geometry(handle);

	//give it requested geometry, but place in center
	view->x = (active_workspace->width - geometry->size.w) / 2;
	view->y = (active_workspace->height- geometry->size.h) / 2;
	view->width = geometry->size.w;
	view->height = geometry->size.h;

	view->desired_width = view->width;
	view->desired_height = view->height;

	view->is_floating = true;

	// Case of focused workspace, just create as child of it
	list_add(active_workspace->floating, view);
	view->parent = active_workspace;
	if (active_workspace->focused == NULL) {
		set_focused_container_for(active_workspace, view);
	}
	return view;
}

/* Destroy container */

swayc_t *destroy_output(swayc_t *output) {
	if (output->children->length == 0) {
		// TODO move workspaces to other outputs
	}
	sway_log(L_DEBUG, "OUTPUT: Destroying output '%lu'", output->handle);
	free_swayc(output);
	return &root_container;
}

swayc_t *destroy_workspace(swayc_t *workspace) {
	// NOTE: This is called from elsewhere without checking children length
	// TODO move containers to other workspaces?
	// for now just dont delete
	
	// Do not destroy this if it's the last workspace on this output
	swayc_t *output = workspace->parent;
	while (output && output->type != C_OUTPUT) {
		output = output->parent;
	}
	if (output) {
		if (output->children->length == 1) {
			return NULL;
		}
	}

	if (workspace->children->length == 0) {
		sway_log(L_DEBUG, "Workspace: Destroying workspace '%s'", workspace->name);
		swayc_t *parent = workspace->parent;
		free_swayc(workspace);
		return parent;
	}
	return NULL;
}

swayc_t *destroy_container(swayc_t *container) {
	while (container->children->length == 0 && container->type == C_CONTAINER) {
		sway_log(L_DEBUG, "Container: Destroying container '%p'", container);
		swayc_t *parent = container->parent;
		free_swayc(container);
		container = parent;
	}
	return container;
}

swayc_t *destroy_view(swayc_t *view) {
	if (view == NULL) {
		sway_log(L_DEBUG, "Warning: NULL passed into destroy_view");
		return NULL;
	}
	sway_log(L_DEBUG, "Destroying view '%p'", view);
	swayc_t *parent = view->parent;
	free_swayc(view);

	// Destroy empty containers
	if (parent->type == C_CONTAINER) {
		return destroy_container(parent);
	}
	return parent;
}

swayc_t *find_container(swayc_t *container, bool (*test)(swayc_t *view, void *data), void *data) {
	if (!container->children) {
		return NULL;
	}
	// Special case for checking floating stuff
	int i;
	if (container->type == C_WORKSPACE) {
		for (i = 0; i < container->floating->length; ++i) {
			swayc_t *child = container->floating->items[i];
			if (test(child, data)) {
				return child;
			}
		}
	}
	for (i = 0; i < container->children->length; ++i) {
		swayc_t *child = container->children->items[i];
		if (test(child, data)) {
			return child;
		} else {
			swayc_t *res = find_container(child, test, data);
			if (res) {
				return res;
			}
		}
	}
	return NULL;
}

void container_map(swayc_t *container, void (*f)(swayc_t *view, void *data), void *data) {
	if (!container || !container->children || !container->children->length)  {
		return;
	}
	int i;
	for (i = 0; i < container->children->length; ++i) {
		swayc_t *child = container->children->items[i];
		f(child, data);
		container_map(child, f, data);
	}
	if (container->type == C_WORKSPACE) {
		for (i = 0; i < container->floating->length; ++i) {
			swayc_t *child = container->floating->items[i];
			f(child, data);
			container_map(child, f, data);
		}
	}
}

void set_view_visibility(swayc_t *view, void *data) {
	uint32_t *p = data;
	if (view->type == C_VIEW) {
		wlc_view_set_mask(view->handle, *p);
		if (*p == 2) {
			wlc_view_bring_to_front(view->handle);
		} else {
			wlc_view_send_to_back(view->handle);
		}
	}
	view->visible = (*p == 2);
}

void reset_gaps(swayc_t *view, void *data) {
	if (view->type == C_OUTPUT) {
		view->gaps = config->gaps_outer;
	}
	if (view->type == C_VIEW) {
		view->gaps = config->gaps_inner;
	}
}
