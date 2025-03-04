/*
 * Copyright (c) 2000, 2003, 2010, 2019 Greg Haerr <greg@censoft.com>
 * Copyright (c) 1991 David I. Bell
 *
 * Graphics server event routines for windows.
 */
#include <stdlib.h>
#include <string.h>
#include "serv.h"
#include "osdep.h"

/* these were required for VNC event mutex */
#define EVENT_LOCK_DECLARE(m)
#define EVENT_LOCK(m)
#define EVENT_UNLOCK(m)

/* readable error strings*/
const char *nxErrorStrings[] = {
	GR_ERROR_STRINGS
};

/*
 * Generate an error from a graphics function.
 * This creates a special event which describes the error.
 * Only one error event at a time can be saved for delivery to a client.
 * This is ok since there are usually lots of redundant errors generated
 * before the client can notice, errors occurs after the fact, and clients
 * can't do much about them except complain and die.  The error is saved
 * specially so that memory problems cannot occur.
 */
void GsError(GR_ERROR code, GR_ID id)
{
	GR_EVENT_ERROR	*ep;		/* event to describe error */

	EPRINTF("nano-X: GsError ");
	if(curfunc)
		EPRINTF("in %s: ", curfunc);
	EPRINTF(nxErrorStrings[code], id);

	/* if no clients, nothing to report*/
	if (!curclient)
		return;

	/* 
	 * If we ran out of memory, another call to GsAllocEvent will
	 * simply get us back here, so don't bother trying to report the event.
	 */
	if (code == GR_ERROR_MALLOC_FAILED)
		return;

	/* queue the error event regardless of GrSelectEvents*/
	ep = (GR_EVENT_ERROR *)GsAllocEvent(curclient);
	ep->type = GR_EVENT_TYPE_ERROR;
	ep->name[0] = 0;
	if(curfunc)
		strcpy(ep->name, curfunc);
	ep->code = code;
	ep->id = id;
}

/*
 * Allocate an event to be passed back to the specified client.
 * The event is already chained onto the event queue, and only
 * needs filling out.  Returns NULL with an error generated if
 * the event cannot be allocated.
 */
GR_EVENT *GsAllocEvent(GR_CLIENT *client)
{
	GR_EVENT_LIST	*elp;		/* current element list */
	GR_CLIENT	*oldcurclient;	/* old current client */

	/*
	 * Get a new event structure from the free list, or else
	 * allocate it using malloc.
	 */
	EVENT_UNLOCK(&eventMutex);
	elp = eventfree;
	if (elp)
		eventfree = elp->next;
	else {
		elp = (GR_EVENT_LIST *) malloc(sizeof(GR_EVENT_LIST));
		if (elp == NULL) {
			oldcurclient = curclient;
			curclient = client;
			GsError(GR_ERROR_MALLOC_FAILED, 0);
			curclient = oldcurclient;
			EVENT_UNLOCK(&eventMutex);
			return NULL;
		}
	}

	/*
	 * Add the event to the end of the event list.
	 */
	if (!client->eventhead)
	  client->eventhead = elp;
	if (client->eventtail)
		client->eventtail->next = elp;
	client->eventtail = elp;

	elp->next = NULL;
	elp->event.type = GR_EVENT_TYPE_NONE;

	EVENT_UNLOCK(&eventMutex);
	return &elp->event;
}

/*
 * Update mouse status and issue events on it if necessary.
 * This function doesn't block, but is only called when Poll() returns TRUE
 * or GsSelect shows some data waiting to be read on the mouse file descriptor.
 */
GR_BOOL GsCheckMouseEvent(void)
{
	GR_COORD	rootx;		/* latest mouse x position */
	GR_COORD	rooty;		/* latest mouse y position */
	int		newbuttons;	/* latest buttons */
	int		mousestatus;	/* latest mouse status */

	if (mousedev.Poll && (mousedev.Poll() == 0))
			return FALSE;

	/* Read the latest mouse status*/
	mousestatus = GdReadMouse(&rootx, &rooty, &newbuttons);
	if (mousestatus <= 0)
	{
		if (mousestatus == MOUSE_FAIL)
			GsError(GR_ERROR_MOUSE_ERROR, 0);
		return FALSE;
	}

	/* Deliver events as appropriate*/
	GsHandleMouseStatus(rootx, rooty, newbuttons);

	/* possibly reset portrait mode based on mouse position*/
	if (autoportrait)
		GsSetPortraitModeFromXY(rootx, rooty);
	return TRUE;
}

/*
 * Update keyboard status and issue events on it if necessary.
 * This function doesn't block, but is only called when Poll() returns TRUE
 * or GsSelect shows some data waiting to be read on the keyboard file descriptor.
 */
GR_BOOL GsCheckKeyboardEvent(void)
{
	MWKEY	 	mwkey;		/* latest character */
	MWKEYMOD 	modifiers;	/* latest modifiers */
	MWSCANCODE	scancode;
	int	 	keystatus;	/* latest keyboard status */

	if (kbddev.Poll && (kbddev.Poll() == 0))
			return FALSE;

	/* Read the latest keyboard status: */
	keystatus = GdReadKeyboard(&mwkey, &modifiers, &scancode);

	if (keystatus <= 0)
	{
		if (keystatus == KBD_QUIT)	/* special case for quit message*/
			GsTerminate();
		if (keystatus == KBD_FAIL)
			GsError(GR_ERROR_KEYBOARD_ERROR, 0);
		return FALSE;				/* read failed or no new data*/
	}

	/* handle special keys*/
	switch (mwkey)
	{
	case MWKEY_QUIT:
#if DEBUG
		if (modifiers & MWKMOD_CTRL)
			GdCaptureScreen(NULL, "screen.bmp");
		else 
#endif
		GsTerminate();
		break;
	case MWKEY_REDRAW:
		GsRedrawScreen();
		break;
#if DEBUG
	case MWKEY_PRINT:
		if (keystatus == KBD_KEYPRESS)
			GdCaptureScreen(NULL, "screen.bmp");
		break;
#endif
	}

	/* Deliver events as appropriate*/
	GsDeliverKeyboardEvent(0, (keystatus == KBD_KEYPRESS?
		GR_EVENT_TYPE_KEY_DOWN: GR_EVENT_TYPE_KEY_UP),
		mwkey, modifiers, scancode);
	return TRUE;
}

/*
 * Handle all mouse events.  These are mouse enter, mouse exit, mouse
 * motion, mouse position, button down, and button up.  This also moves
 * the cursor to the new mouse position and changes it shape if needed.
 */
void GsHandleMouseStatus(GR_COORD newx, GR_COORD newy, int newbuttons)
{
	int	 changebuttons;	/* buttons that have changed */
	MWKEYMOD modifiers;	/* latest modifiers */
	
	GdGetModifierInfo(NULL, &modifiers); /* Read kbd modifiers */

#if !MW_FEATURE_TINY
	/* If we are currently in raw mode, then just deliver the raw event */
	if (mousedev.flags & MOUSE_RAW) { 
		GsDeliverRawMouseEvent(newx, newy, newbuttons, modifiers);
		return;
	}
#endif

	/*
	 * First, if the mouse has moved, then position the cursor to the
	 * new location, which will send mouse enter, mouse exit, focus in,
	 * and focus out events if needed.  Check here for mouse motion and
	 * mouse position events.  Flush the device queue to make sure the
	 * new cursor location is quickly seen by the user.
	 */
	if (newx != cursorx || newy != cursory) {
		GsResetScreenSaver();
		GrMoveCursor(newx, newy);
		GsDeliverMotionEvent(GR_EVENT_TYPE_MOUSE_MOTION, newbuttons, modifiers);
		GsDeliverMotionEvent(GR_EVENT_TYPE_MOUSE_POSITION, newbuttons, modifiers);
	}

	/*
	 * Next, generate a button up event if any buttons have been released.
	 */
	changebuttons = (curbuttons & ~newbuttons);
	if (changebuttons) {
	  GsResetScreenSaver();
	  GsDeliverButtonEvent(GR_EVENT_TYPE_BUTTON_UP, newbuttons,
		changebuttons, modifiers);
	}

	/*
	 * Generate a button down event if any buttons have been pressed.
	 */
	changebuttons = (~curbuttons & newbuttons);
	if (changebuttons) {
		GsResetScreenSaver();
		GsCheckMouseWindow();	/* fixes occasional lost window move when mousewp=root*/
		GsDeliverButtonEvent(GR_EVENT_TYPE_BUTTON_DOWN, newbuttons,
			changebuttons, modifiers);
	}

	/*
	 * Continue sending scroll wheel buttons, unless just changed.
	 */
	if (newbuttons & (GR_BUTTON_SCROLLUP|GR_BUTTON_SCROLLDN)) {
		if (!changebuttons)
			GsDeliverButtonEvent(GR_EVENT_TYPE_BUTTON_DOWN, newbuttons,
				0, modifiers);
	}

	curbuttons = newbuttons;
}

/*
 * Deliver a mouse button event to the clients which have selected for it.
 * Each client can only be delivered one instance of the event.  The window
 * the event is delivered for is either the smallest one containing the
 * mouse coordinates, or else one of its direct ancestors.  The lowest
 * window in that tree which has enabled for the event gets it.  This scan
 * is done independently for each client.  If a window with the correct
 * noprop mask is reached, or if no window selects for the event, then the
 * event is discarded for that client.  Special case: for the first client
 * that is enabled for both button down and button up events in a window,
 * then the pointer is implicitly grabbed by that window when a button is
 * pressed down in that window.  The grabbing remains until all buttons are
 * released.  While the pointer is grabbed, no other clients or windows can
 * receive button down or up events.
 */
void GsDeliverButtonEvent(GR_EVENT_TYPE type, int buttons, int changebuttons,
			int modifiers)
{
	GR_EVENT_BUTTON	*ep;		/* mouse button event */
	GR_WINDOW	*wp;		/* current window */
	GR_EVENT_CLIENT	*ecp;		/* current event client */
	GR_CLIENT	*client;	/* current client */
	GR_WINDOW_ID	subwid;		/* subwindow id event is for */
	GR_EVENT_MASK	eventmask;	/* event mask */
	GR_EVENT_MASK	tempmask;	/* to get around compiler bug */

	eventmask = GR_EVENTMASK(type);
	if (eventmask == 0)
		return;

	/*
	 * If the pointer is implicitly grabbed, then the only window
	 * which can receive button events is that window.  Otherwise
	 * the window the pointer is in gets the events.  Determine the
	 * subwindow by seeing if it is a child of the grabbed button.
	 */
	wp = mousewp;
	subwid = wp->id;

	if (grabbuttonwp) {
#if 0
		while ((wp != rootwp) && (wp != grabbuttonwp))
			wp = wp->parent;
		if (wp != grabbuttonwp)
			subwid = grabbuttonwp->id;
#endif
		wp = grabbuttonwp;
	}

	for (;;) {
		for (ecp = wp->eventclients; ecp; ecp = ecp->next) {
			if ((ecp->eventmask & eventmask) == 0)
				continue;

			client = ecp->client;

			/*
			 * If this is a button down, the buttons are not
			 * yet grabbed, and this client is enabled for both
			 * button down and button up events, then implicitly
			 * grab the window for him.
			 */
			if ((type == GR_EVENT_TYPE_BUTTON_DOWN) &&
			    (grabbuttonwp == NULL)) {
				tempmask = GR_EVENT_MASK_BUTTON_UP;
				if (ecp->eventmask & tempmask) {
					//EPRINTF("Implicit grab on window %d\n", wp->id);
					grabbuttonwp = wp;
				}
			}

			ep = (GR_EVENT_BUTTON *) GsAllocEvent(client);
			if (ep == NULL)
				continue;

			ep->type = type;
			ep->wid = wp->id;
			ep->subwid = subwid;
			ep->rootx = cursorx;
			ep->rooty = cursory;
			ep->x = cursorx - wp->x;
			ep->y = cursory - wp->y;
			ep->buttons = buttons;
			ep->changebuttons = changebuttons;
			ep->modifiers = modifiers;
			ep->time = GdGetTickCount();
		}

		/*
		 * Events do not propagate if the window was grabbed.
		 * Also release the grab if the buttons are now all released,
		 * which can cause various events.
		 */
		if (grabbuttonwp) {
			if (buttons == 0) {
				//EPRINTF("Implicit ungrab on window %d\n", grabbuttonwp->id);
				grabbuttonwp = NULL;
				GrMoveCursor(cursorx, cursory);
			}
			return;
		}

		if (wp == rootwp || (wp->nopropmask & eventmask))
			return;

		wp = wp->parent;
	}
}

/*
 * Deliver a mouse motion event to the clients which have selected for it.
 * Each client can only be delivered one instance of the event.  The window
 * the event is delivered for is either the smallest one containing the
 * mouse coordinates, or else one of its direct ancestors.  The lowest
 * window in that tree which has enabled for the event gets it.  This scan
 * is done independently for each client.  If a window with the correct
 * noprop mask is reached, or if no window selects for the event, then the
 * event is discarded for that client.  Special case: If the event type is
 * GR_EVENT_TYPE_MOUSE_POSITION, then only the last such event is queued for
 * any single client to reduce events.  If the mouse is implicitly grabbed,
 * then only the grabbing window receives the events, and continues to do
 * so even if the mouse is currently outside of the grabbing window.
 */
void GsDeliverMotionEvent(GR_EVENT_TYPE type, int buttons, MWKEYMOD modifiers)
{
	GR_EVENT_MOUSE	*ep;		/* mouse motion event */
	GR_WINDOW	*wp;		/* current window */
	GR_EVENT_CLIENT	*ecp;		/* current event client */
	GR_CLIENT	*client;	/* current client */
	GR_WINDOW_ID	subwid;		/* subwindow id event is for */
	GR_EVENT_MASK	eventmask;	/* event mask */

	eventmask = GR_EVENTMASK(type);
	if (eventmask == 0)
		return;

	wp = mousewp;
	subwid = wp->id;

	if (grabbuttonwp) {
#if 0
		while ((wp != rootwp) && (wp != grabbuttonwp))
			wp = wp->parent;
		if (wp != grabbuttonwp)
			subwid = grabbuttonwp->id;
#endif
		wp = grabbuttonwp;
	}

	for (;;) {
		for (ecp = wp->eventclients; ecp; ecp = ecp->next) {
			if ((ecp->eventmask & eventmask) == 0)
				continue;

			client = ecp->client;

			/*
			 * If the event is for just the latest position,
			 * then search the event queue for an existing
			 * event of this type (if any), and free it.
			 */
			if (type == GR_EVENT_TYPE_MOUSE_POSITION)
				GsFreePositionEvent(client, wp->id, subwid);

			ep = (GR_EVENT_MOUSE *) GsAllocEvent(client);
			if (ep == NULL)
				continue;

			ep->type = type;
			ep->wid = wp->id;
			ep->subwid = subwid;
			ep->rootx = cursorx;
			ep->rooty = cursory;
			ep->x = cursorx - wp->x;
			ep->y = cursory - wp->y;
			ep->buttons = buttons;
			ep->modifiers = modifiers;
		}

		if (wp == rootwp || grabbuttonwp || (wp->nopropmask & eventmask))
			return;

		wp = wp->parent;
	}
}

/*
 * Deliver a keyboard event to one of the clients which have selected for it.
 * Only the first client found gets the event (no duplicates are sent).  The
 * window the event is delivered to is either the smallest one containing
 * the mouse coordinates, or else one of its direct ancestors (if such a
 * window is a descendant of the focus window), or else just the focus window.
 * The lowest window in that tree which has enabled for the event gets it.
 * If a window with the correct noprop mask is reached, or if no window selects
 * for the event, then the event is discarded.
 */
void GsDeliverKeyboardEvent(GR_WINDOW_ID wid, GR_EVENT_TYPE type,
	GR_KEY keyvalue, GR_KEYMOD modifiers, GR_SCANCODE scancode)
{
	GR_EVENT_KEYSTROKE	*ep;		/* keystroke event */
	GR_WINDOW		*wp;		/* current window */
	GR_WINDOW		*tempwp;	/* temporary window pointer */
	GR_EVENT_CLIENT		*ecp;		/* current event client */
	GR_WINDOW_ID		subwid;		/* subwindow id event is for */
	GR_EVENT_MASK		eventmask;	/* event mask */
	GR_WINDOW		*kwp;
	GR_GRABBED_KEY		*keygrab;

	eventmask = GR_EVENTMASK(type);
	if (eventmask == 0)
		return;

	GsResetScreenSaver();

	/* Check for grabbed keystroke.
	 * - GR_GRAB_HOTKEY events are sent (possibly multiple times) here,
	 *   and the loop terminates normally with keygrab==NULL so the
	 *   event is also delivered normally.
	 * - GR_GRAB_HOTKEY_EXCLUSIVE sends the hotkey events then returns.
	 * - Other exclusive events (GR_GRAB_EXCLUSIVE_MOUSE and GR_GRAB_EXCLUSIVE)
	 *   cause the loop to terminate with keygrab != NULL.  The checking
	 *   for these events happens after the loop.
	 *
	 * Note: This algorithm requires any GR_GRAB_HOTKEY grabs to be
	 * listed _after_ any exclusive grabs for the same key.  The
	 * GrGrabKey() and GrUngrabKey() methods ensure this property holds.
	 */
	for (keygrab = list_grabbed_keys; keygrab != NULL; keygrab = keygrab->next) {
		if (keygrab->key == keyvalue) {
			if ((keygrab->type == GR_GRAB_HOTKEY) ||
			    (keygrab->type == GR_GRAB_HOTKEY_EXCLUSIVE)) {
				ep = (GR_EVENT_KEYSTROKE *) GsAllocEvent(keygrab->owner);
				if (ep == NULL)
					continue;

				ep->type = type;
				ep->wid = keygrab->wid;
				ep->subwid = keygrab->wid;
				ep->rootx = cursorx;
				ep->rooty = cursory;
				ep->x = cursorx;
				ep->y = cursory;
				ep->buttons = curbuttons;
				ep->modifiers = modifiers;
				ep->ch = keyvalue;
				ep->scancode = scancode;
				ep->hotkey = GR_TRUE;
				if (keygrab->type == GR_GRAB_HOTKEY_EXCLUSIVE)
					return;	/* only one client gets it */
			} else {
				/* GR_GRAB_EXCLUSIVE or GR_GRAB_EXCLUSIVE_MOUSE */
				break; /* found it, exit the loop. */
			}
		}
	}

	/* Handle a grabbed key:
	 * The associated window must be an ancestor of the focused window,
	 * or (for GR_GRAB_EXCLUSIVE_MOUSE only) a descendent that contains the
	 * pointer.
	 */
	if (keygrab != NULL) {
		/* The key grab must be of type GR_GRAB_EXCLUSIVE or
		 * GR_GRAB_EXCLUSIVE_MOUSE
		 */

		/* Find the window that has the grab */
		wp = GsFindWindow(keygrab->wid);
		if (wp == NULL)
			return; /* Key is reserved by window that doesn't exist. */

		/* See if the grabbing window is an ancestor of the focussed window. */
		kwp = focuswp;
		while (kwp != wp && kwp != rootwp)
			kwp = kwp->parent;

		/* Want to send event if:
		 * GR_GRAB_EXCLUSIVE: grabbing window is an ancestor of focussed window
		 * GR_GRAB_EXCLUSIVE_MOUSE: same as GR_GRAB_EXCLUSIVE OR
		 * the mouse is in the grabbing window.
		 */
		if (kwp != wp && (keygrab->type != GR_GRAB_EXCLUSIVE_MOUSE ||
		    wp != mousewp))
			return;

		subwid = wp->id;
	} else {
		/* if window id passed, use it, otherwise focus window */
		if (wid) {
			kwp = GsFindWindow(wid);
			if (!kwp)
				return;
		} else
			kwp = focuswp;
		wp = mousewp;
		subwid = wp->id;

		/*
		 * See if the actual window the pointer is in is a descendant of
		 * the focus window.  If not, then ignore it, and force the input
		 * into the focus window itself.
		 */
		tempwp = wp;
		while (tempwp != kwp && tempwp != rootwp)
			tempwp = tempwp->parent;

		if (tempwp != kwp) {
			wp = kwp;
			subwid = wp->id;
		}
	}

	/*
	 * Now walk upwards looking for the first window which will accept
	 * the keyboard event.  However, do not go beyond the focus window,
	 * and only give the event to one client.
	 */
	for (;;) {
		for (ecp = wp->eventclients; ecp; ecp = ecp->next) {

			if ((ecp->eventmask & eventmask) == 0)
				continue;

			ep = (GR_EVENT_KEYSTROKE *) GsAllocEvent(ecp->client);
			if (ep == NULL)
				return;

			ep->type = type;
			ep->wid = wp->id;
			ep->subwid = subwid;
			ep->rootx = cursorx;
			ep->rooty = cursory;
			ep->x = cursorx - wp->x;
			ep->y = cursory - wp->y;
			ep->buttons = curbuttons;
			ep->modifiers = modifiers;
			ep->ch = keyvalue;
			ep->scancode = scancode;
			ep->hotkey = GR_FALSE;
			return;			/* only one client gets it */
		}

		if (wp == rootwp || wp == kwp || (wp->nopropmask & eventmask))
				return;

		wp = wp->parent;
	}
}

/*
 * Try to deliver a exposure event to the clients which have selected for it.
 * This does not send exposure events for unmapped or input-only windows.
 * Exposure events do not propagate upwards.
 */
void
GsDeliverExposureEvent(GR_WINDOW *wp, GR_COORD x, GR_COORD y,
	GR_SIZE width, GR_SIZE height)
{
	GR_EVENT_EXPOSURE	*ep;		/* exposure event */
	GR_EVENT_CLIENT		*ecp;		/* current event client */

	if (!wp->realized || !wp->output)
		return;

	for (ecp = wp->eventclients; ecp; ecp = ecp->next) {
		if ((ecp->eventmask & GR_EVENT_MASK_EXPOSURE) == 0)
			continue;

		GsFreeExposureEvent(ecp->client, wp->id, x, y, width, height);

		ep = (GR_EVENT_EXPOSURE *) GsAllocEvent(ecp->client);
		if (ep == NULL)
			continue;

		ep->type = GR_EVENT_TYPE_EXPOSURE;
		ep->wid = wp->id;
		ep->x = x;
		ep->y = y;
		ep->width = width;
		ep->height = height;
	}
}

/*
 * Search for an enclosed expose event in the specified client's
 * event queue, and remove it.  This is used to prevent multiple expose
 * events from being delivered, thus providing a more pleasing visual
 * redraw effect than if the events were all sent.
 */
void
GsFreeExposureEvent(GR_CLIENT *client, GR_WINDOW_ID wid, GR_COORD x,
	GR_COORD y, GR_SIZE width, GR_SIZE height)
{
	GR_EVENT_LIST	*elp;		/* current element list */
	GR_EVENT_LIST	*prevelp;	/* previous element list */

	prevelp = NULL;
	for (elp = client->eventhead; elp; prevelp = elp, elp = elp->next) {
		if (elp->event.type != GR_EVENT_TYPE_EXPOSURE ||
		    elp->event.exposure.wid != wid)
			continue;
		if (elp->event.exposure.x < x || elp->event.exposure.y < y ||
		    elp->event.exposure.x+elp->event.exposure.width > x+width ||
		    elp->event.exposure.y+elp->event.exposure.height > y+height)
			continue;

		/*
		 * Found one, remove it and put it back on the free list.
		 */
		if (prevelp)
			prevelp->next = elp->next;
		else
			client->eventhead = elp->next;
		if (client->eventtail == elp)
			client->eventtail = prevelp;

		elp->next = eventfree;
		eventfree = elp;
		return;
	}
}

/*
 * Try to deliver an update event to the clients which have selected for it.
 */
void
GsDeliverUpdateEvent(GR_WINDOW *wp, GR_UPDATE_TYPE utype, GR_COORD x,
	GR_COORD y, GR_SIZE width, GR_SIZE height)
{
	GR_EVENT_MASK		cmask = GR_EVENT_MASK_UPDATE;
	GR_EVENT_UPDATE		*ep;		/* update event */
	GR_EVENT_CLIENT		*ecp;		/* current event client */
	GR_WINDOW_ID		id = wp->id;
	GR_WINDOW			*orgwp;
	int			lcount = 0;

	/* adjust reported x,y to be parent-relative*/
	if (wp->parent) {
		x -= wp->parent->x;
		y -= wp->parent->y;
	}

update_again:
	for (ecp = wp->eventclients; ecp; ecp = ecp->next) {
		if ((ecp->eventmask & cmask) == 0)
			continue;

#if NANOWM
		/* Ensure we're processing root child update map event for window owner*/
		if (lcount && wp->id == GR_ROOT_WINDOW_ID && utype == GR_UPDATE_MAP) {
			/*DPRINTF("wid %d subwid %d  %d,%d\n", wp->id, id,
				orgwp->owner->id, ecp->client->id);*/
			if (orgwp->owner->id != ecp->client->id) {
				/*DPRINTF("SKIP CHLD MAP %d: %d %d\n", id,
					orgwp->owner->id, ecp->client->id);*/
				continue;
			}
		}
#endif
		ep = (GR_EVENT_UPDATE *) GsAllocEvent(ecp->client);
		if (ep == NULL)
			continue;

		ep->type = lcount?  GR_EVENT_TYPE_CHLD_UPDATE: GR_EVENT_TYPE_UPDATE;
		ep->utype = utype;
		ep->wid = wp->id;	/* GrSelectEvents window id*/
		ep->subwid = id;	/* update window id*/
		ep->x = x;
		ep->y = y;
		ep->width = width;
		ep->height = height;
	}

	/* If we are currently checking the window updated, go back and 
	 * check its parent too */
	if (!lcount++) {	
		orgwp = wp;
		wp = wp->parent;
		/* check for NULL on root window id*/
		if (wp == NULL)
			return;
		cmask = GR_EVENT_MASK_CHLD_UPDATE;
		goto update_again;
	}
}

/*
 * Try to deliver a general event such as focus in, focus out, mouse enter,
 * or mouse exit to the clients which have selected for it.  These events
 * do not propagate upwards.
 */
void
GsDeliverGeneralEvent(GR_WINDOW *wp, GR_EVENT_TYPE type, GR_WINDOW *other)
{
	GR_EVENT_GENERAL	*gp;		/* general event */
	GR_EVENT_CLIENT		*ecp;		/* current event client */
	GR_EVENT_MASK		eventmask;	/* event mask */

	eventmask = GR_EVENTMASK(type);
	if (eventmask == 0)
		return;

	for (ecp = wp->eventclients; ecp; ecp = ecp->next) {
		if ((ecp->eventmask & eventmask) == 0)
			continue;

		gp = (GR_EVENT_GENERAL *) GsAllocEvent(ecp->client);
		if (gp == NULL)
			continue;

		gp->type = type;
		gp->wid = wp->id;
		if (other)
			gp->otherid = other->id;
		else gp->otherid = 0;

		/* add root window x, y mouse coordinates*/
		gp->rootx = cursorx;
		gp->rooty = cursory;

		/* window x,y only valid for mouse enter*/
		gp->x = cursorx - wp->x;
		gp->y = cursory - wp->y;
	}
}

/*
 * Deliver a portrait mode changed event to all windows which
 * have selected for it.
 */
void
GsDeliverPortraitChangedEvent(void)
{
	GR_WINDOW		*wp;
	GR_EVENT_GENERAL	*gp;
	GR_EVENT_CLIENT		*ecp;

	for (wp=listwp; wp; wp=wp->next) {
		for (ecp = wp->eventclients; ecp; ecp = ecp->next) {
			if ((ecp->eventmask & GR_EVENT_MASK_PORTRAIT_CHANGED) == 0)
				continue;

			gp = (GR_EVENT_GENERAL *) GsAllocEvent(ecp->client);
			if (gp == NULL)
				continue;

			gp->type = GR_EVENT_TYPE_PORTRAIT_CHANGED;
			gp->wid = wp->id;
			gp->otherid = 0;
		}
	}
}

/*
 * Deliver a Screen Saver event. There is only one parameter- activate the
 * screen saver or deactivate it. We only deliver it to the root window,
 * but we do send it to every client which has selected for it (because the
 * program which starts the screen saver on an activate event might not also
 * be the screen saver program which wants to catch the deactivate event).
 */
void
GsDeliverScreenSaverEvent(GR_BOOL activate)
{
	GR_EVENT_SCREENSAVER	*gp;		/* screensaver event */
	GR_EVENT_CLIENT		*ecp;		/* current event client */

	for (ecp = rootwp->eventclients; ecp; ecp = ecp->next) {
		if ((ecp->eventmask & GR_EVENT_MASK_SCREENSAVER) == 0)
			continue;

		gp = (GR_EVENT_SCREENSAVER *) GsAllocEvent(ecp->client);
		if (gp == NULL)
			continue;

		gp->type = GR_EVENT_TYPE_SCREENSAVER;
		gp->activate = activate;
	}
}

/*
 * Deliver a client data request event. Delivered to the clients who have
 * selected for this event on the specified window only.
 */
void
GsDeliverClientDataReqEvent(GR_WINDOW_ID wid, GR_WINDOW_ID rid,
	GR_SERIALNO serial, GR_MIMETYPE mimetype)
{
	GR_EVENT_CLIENT_DATA_REQ *gp;		/* client data request event */
	GR_EVENT_CLIENT		*ecp;		/* current event client */
	GR_WINDOW *wp;

	if(!(wp = GsFindWindow(wid)))
		return;

	for (ecp = wp->eventclients; ecp; ecp = ecp->next) {
		if ((ecp->eventmask & GR_EVENT_MASK_CLIENT_DATA_REQ) == 0)
			continue;

		gp = (GR_EVENT_CLIENT_DATA_REQ *) GsAllocEvent(ecp->client);
		if (gp == NULL)
			continue;

		gp->type = GR_EVENT_TYPE_CLIENT_DATA_REQ;
		gp->wid = wid;
		gp->rid = rid;
		gp->serial = serial;
		gp->mimetype = mimetype;
		continue;
	}
}

/*
 * Deliver a client data event. Delivered to the clients who have selected for
 * this event on the specified window only.
 */
void
GsDeliverClientDataEvent(GR_WINDOW_ID wid, GR_WINDOW_ID rid,
	GR_SERIALNO serial, GR_LENGTH len, GR_LENGTH thislen, void *data)
{
	GR_EVENT_CLIENT_DATA *gp;		/* client data request event */
	GR_EVENT_CLIENT		*ecp;		/* current event client */
	GR_WINDOW *wp;

	if(!(wp = GsFindWindow(wid)))
		return;

	for (ecp = wp->eventclients; ecp; ecp = ecp->next) {
		if ((ecp->eventmask & GR_EVENT_MASK_CLIENT_DATA) == 0)
			continue;

		gp = (GR_EVENT_CLIENT_DATA *) GsAllocEvent(ecp->client);
		if (gp == NULL)
			continue;

		gp->type = GR_EVENT_TYPE_CLIENT_DATA;
		gp->wid = wid;
		gp->rid = rid;
		gp->serial = serial;
		gp->len = len;
		gp->datalen = thislen;
		if(!(gp->data = malloc(thislen))) {
			GsError(GR_ERROR_MALLOC_FAILED, wid);
			return;
		}
		memcpy(gp->data, data, thislen);
		continue;
	}
}

/*
 * Search for a matching mouse position event in the specified client's
 * event queue, and remove it.  This is used to prevent multiple position
 * events from being delivered, thus providing a more efficient rubber-
 * banding effect than if the mouse motion events were all sent.
 */
void
GsFreePositionEvent(GR_CLIENT *client, GR_WINDOW_ID wid, GR_WINDOW_ID subwid)
{
	GR_EVENT_LIST	*elp;		/* current element list */
	GR_EVENT_LIST	*prevelp;	/* previous element list */

	EVENT_LOCK(&eventMutex);
	prevelp = NULL;
	for (elp = client->eventhead; elp; prevelp = elp, elp = elp->next) {
		if (elp->event.type != GR_EVENT_TYPE_MOUSE_POSITION)
			continue;
		if (elp->event.mouse.wid != wid)
			continue;
		if (elp->event.mouse.subwid != subwid)
			continue;

		/*
		 * Found one, remove it and put it back on the free list.
		 */
		if (prevelp)
			prevelp->next = elp->next;
		else
			client->eventhead = elp->next;
		if (client->eventtail == elp)
			client->eventtail = prevelp;

		elp->next = eventfree;
		eventfree = elp;
		break;
	}
	EVENT_UNLOCK(&eventMutex);
}

/*
 * Deliver a "selection owner changed" event to all windows which have
 * selected for it. We deliver this event to all clients which have selected
 * to receive GR_EVENT_TYPE_SELECTION_CHANGED events for the window of the
 * _previous_ selection owner.
 */
void
GsDeliverSelectionChangedEvent(GR_WINDOW_ID old_owner, GR_WINDOW_ID new_owner)
{
	GR_EVENT_SELECTION_CHANGED *gp;		/* selection changed event */
	GR_EVENT_CLIENT		*ecp;		/* current event client */
	GR_WINDOW *wp;

	if(!(wp = GsFindWindow(old_owner)))
		return;

	for (ecp = wp->eventclients; ecp; ecp = ecp->next) {
		if ((ecp->eventmask & GR_EVENT_MASK_SELECTION_CHANGED) == 0)
			continue;

		gp = (GR_EVENT_SELECTION_CHANGED *) GsAllocEvent(ecp->client);
		if (gp == NULL)
			continue;

		gp->type = GR_EVENT_TYPE_SELECTION_CHANGED;
		gp->new_owner = new_owner;
	}
}

#if !MW_FEATURE_TINY
/* This is a bit of a misnomer - this will deliver the normal events
   but it doesn't bother doing any sort of bounds checking or anything,
   we just start at the "focus" window and try to deliver events to the path
*/
void
GsDeliverRawMouseEvent(GR_COORD rx, GR_COORD ry, int buttons, int modifiers)
{
	int i;

	GR_WINDOW *wp;		/* current window */
	GR_CLIENT *client;	/* current client */
	GR_WINDOW_ID subwid;	/* subwindow id event is for */

	GR_EVENT_CLIENT *ecp;

	/* Start with the "focus" window and move up */
	/* Since the raw mouse position doesn't match the actual
	   geometry, this is all we can do */
	wp = mousewp;
	subwid = wp->id;

	for (ecp = wp->eventclients; ecp; ecp = ecp->next) {
		GR_EVENT_MOUSE *ep;

		if ((ecp->eventmask & GR_EVENT_MASK_MOUSE_POSITION) == 0)
			continue;

		client = ecp->client;

		GsFreePositionEvent(client, wp->id, subwid);

		ep = (GR_EVENT_MOUSE *) GsAllocEvent(client);
		if (ep == NULL)
			continue;

		ep->type = GR_EVENT_TYPE_MOUSE_POSITION;
		ep->wid = wp->id;
		ep->subwid = subwid;
		ep->rootx = rx;
		ep->rooty = ry;
		ep->x = 0;	/* These make no sense in raw mode */
		ep->y = 0;
		ep->buttons = buttons;
		ep->modifiers = modifiers;

		if ((wp == rootwp) || (wp->nopropmask & GR_EVENT_MASK_MOUSE_POSITION))
			break;

		wp = wp->parent;
	}

	/* Deliver button events if we have to */
	for (i = 0; i < 2; i++) {
		GR_EVENT_BUTTON *gp;
		GR_BUTTON cbuttons = 0;
		GR_EVENT_TYPE etype = (i == 0)?
			GR_EVENT_TYPE_BUTTON_DOWN : GR_EVENT_TYPE_BUTTON_UP;

		if (i == 0)
			cbuttons = (curbuttons & ~buttons);
		else
			cbuttons = (~buttons & curbuttons);
		if (!cbuttons)
			continue;

		wp = mousewp;
		subwid = wp->id;

		for (ecp = wp->eventclients; ecp; ecp = ecp->next) {
			client = ecp->client;

			if ((ecp->eventmask & GR_EVENTMASK(etype)) == 0)
				continue;

			gp = (GR_EVENT_BUTTON *) GsAllocEvent(ecp->client);

			if (gp == NULL)
				continue;

			gp->type = etype;
			gp->wid = wp->id;
			gp->subwid = subwid;
			gp->rootx = rx;
			gp->rooty = ry;
			gp->x = 0;
			gp->y = 0;
			gp->buttons = buttons;
			gp->changebuttons = cbuttons;
			gp->modifiers = modifiers;
			gp->time = GdGetTickCount();

			if ((wp == rootwp) || (wp->nopropmask & GR_EVENTMASK(etype)))
				break;

			wp = wp->parent;
		}
	}

	curbuttons = buttons;
}
#endif

void
GsDeliverTimerEvent(GR_CLIENT * client, GR_WINDOW_ID wid, GR_TIMER_ID tid)
{
	GR_EVENT_TIMER *event;	/* general event */
	GR_EVENT_CLIENT *ecp;	/* current event client */
	GR_WINDOW *wp;		/* current window */

	if ((wp = GsFindWindow(wid)) == NULL)
		return;

	for (ecp = wp->eventclients; ecp != NULL; ecp = ecp->next) {
		if ((ecp->client == client)
		    && ((ecp->eventmask & GR_EVENT_MASK_TIMER) != 0)) {
			event = (GR_EVENT_TIMER *) GsAllocEvent(client);
			if (event == NULL)
				break;

			event->type = GR_EVENT_TYPE_TIMER;
			event->wid = wid;
			event->tid = tid;
		}
	}
}
