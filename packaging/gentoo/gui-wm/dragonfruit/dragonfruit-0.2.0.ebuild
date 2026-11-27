# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit meson

DESCRIPTION="An infinite-canvas Wayland compositor forked from Mango"
HOMEPAGE="https://codeberg.org/central-lambda-group/dragonfruit"

inherit git-r3
EGIT_REPO_URI="https://codeberg.org/central-lambda-group/dragonfruit.git"
if [[ ${PV} == *9999 ]]; then
	EGIT_BRANCH="dragonfruit"
else
	# release = the tag of the same name; a new release is a copy of this file
	EGIT_COMMIT="${PV}"
	KEYWORDS="~amd64 ~arm64"
fi

LICENSE="CC0-1.0 GPL-3+ MIT"
SLOT="0"

DOCS=(
	README.md
	docs/installation.md
)

IUSE="X asan"

RDEPEND="
	>=gui-libs/wlroots-0.20:=[libinput,session,X?]
	<gui-libs/wlroots-0.21:=[X?]
	>=gui-libs/scenefx-0.5:0.5

	dev-libs/cJSON
	dev-libs/glib:2
	dev-libs/libinput:=
	dev-libs/libpcre2:=
	dev-libs/wayland

	media-libs/libglvnd

	x11-libs/cairo
	x11-libs/libdrm
	x11-libs/libxkbcommon
	x11-libs/pango
	x11-libs/pixman

	X? (
		x11-base/xwayland
		x11-libs/libxcb:=
		x11-libs/xcb-util-wm
	)
"

DEPEND="
	${RDEPEND}
	sys-kernel/linux-headers
"

BDEPEND="
	>=dev-build/meson-0.60.0
	>=dev-libs/wayland-protocols-1.32
	>=dev-util/wayland-scanner-1.23
	virtual/pkgconfig
"

src_configure() {
	local emesonargs=(
		$(meson_feature X xwayland)
		$(meson_use asan)
	)

	meson_src_configure
}

pkg_postinst() {
	elog "The stock config was installed to /etc/dragonfruit/config.conf."
	elog "Copy it before editing:"
	elog
	elog "  mkdir -p ~/.config/dragonfruit"
	elog "  cp /etc/dragonfruit/config.conf ~/.config/dragonfruit/config.conf"
	elog
	elog "Validate a config without starting a session:  dragonfruit -c FILE -p"
	elog "Start from a TTY with 'dragonfruit', or pick Dragonfruit at your"
	elog "display manager."
}
