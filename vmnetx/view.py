#
# vmnetx.view - vmnetx GUI
#
# Copyright (C) 2009-2012 Carnegie Mellon University
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of version 2 of the GNU General Public License as published
# by the Free Software Foundation.  A copy of the GNU General Public License
# should have been distributed along with this program in the file
# COPYING.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.
#

import gtk
import gtkvnc
import socket

class VNCWidget(gtkvnc.Display):
    def __init__(self, path):
        gtkvnc.Display.__init__(self)
        self._path = path
        self._sock = None

        self.keyboard_grabbed = False
        self.mouse_grabbed = False
        def sa(wid, attr, value):
            setattr(self, attr, value)
        self.connect('vnc-keyboard-grab', sa, 'keyboard_grabbed', True)
        self.connect('vnc-keyboard-ungrab', sa, 'keyboard_grabbed', False)
        self.connect('vnc-pointer-grab', sa, 'mouse_grabbed', True)
        self.connect('vnc-pointer-ungrab', sa, 'mouse_grabbed', False)
        self.set_pointer_grab(True)
        self.set_keyboard_grab(True)

        # Set initial widget size
        self.set_size_request(640, 480)

    def connect_vnc(self):
        try:
            self._sock = socket.socket(socket.AF_UNIX)
            self._sock.connect(self._path)
            self.open_fd(self._sock.fileno())
        except socket.error:
            self.emit('vnc-disconnected')


class StatusBarWidget(gtk.HBox):
    def __init__(self, vnc):
        gtk.HBox.__init__(self, spacing=3)
        self.pack_start(gtk.Label())  # filler

        theme = gtk.icon_theme_get_default()
        def add_icon(name, sensitive):
            icon = gtk.Image()
            icon.set_from_pixbuf(theme.load_icon(name, 24, 0))
            icon.set_sensitive(sensitive)
            self.pack_start(icon, expand=False)
            return icon

        escape_label = gtk.Label('Ctrl-Alt')
        escape_label.set_alignment(0.5, 0.8)
        escape_label.set_padding(3, 0)
        self.pack_start(escape_label, expand=False)

        keyboard_icon = add_icon('input-keyboard', vnc.keyboard_grabbed)
        mouse_icon = add_icon('input-mouse', vnc.mouse_grabbed)
        vnc.connect('vnc-keyboard-grab', self._grabbed, keyboard_icon, True)
        vnc.connect('vnc-keyboard-ungrab', self._grabbed, keyboard_icon, False)
        vnc.connect('vnc-pointer-grab', self._grabbed, mouse_icon, True)
        vnc.connect('vnc-pointer-ungrab', self._grabbed, mouse_icon, False)

    def _grabbed(self, wid, icon, grabbed):
        icon.set_sensitive(grabbed)


class VMWindow(gtk.Window):
    def __init__(self, name, path):
        gtk.Window.__init__(self)
        self.set_title(name)
        self.connect('delete-event', lambda wid, ev: gtk.main_quit())

        box = gtk.VBox()
        self.add(box)

        self._vnc = VNCWidget(path)
        self._vnc.connect('vnc-desktop-resize', self._vnc_resize)
        self._vnc.connect('vnc-disconnected', gtk.main_quit)
        box.pack_start(self._vnc)

        statusbar = StatusBarWidget(self._vnc)
        box.pack_end(statusbar, expand=False)

    def connect_vnc(self):
        self._vnc.connect_vnc()

    def _vnc_resize(self, wid, width, height):
        # Resize the window to the minimum allowed by its geometry
        # constraints
        self.resize(1, 1)