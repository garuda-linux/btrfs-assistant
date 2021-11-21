# Maintainer: <dr460nf1r3 at garudalinux dot org>
# Maintainer: <librewish@garudalinux.org>

pkgname=btrfs-assistant
pkgver=1.0
pkgrel=1
pkgdesc="An application for managing btrfs subvolumes and snapper snapshots"
arch=('x86_64')
url="https://gitlab.com/garuda-linux/applications/$pkgname"
license=('GPL3')
depends=('qt5-base' 'qt5-svg' 'noto-fonts' 'polkit')
optdepends=('snapper')
makedepends=('git' 'cmake' 'qt5-tools')
groups=('garuda')
source=("$pkgname-$pkgver.tar.gz::$url/-/archive/$pkgver/$pkgname-$pkgver.tar.gz")
md5sums=('SKIP')

build() {
	cd "$srcdir"
	cmake -B build -S "$pkgname-$pkgver" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE='Release'
	make -C build
}

package() {
	make -C build DESTDIR="$pkgdir" install

	cd "$srcdir/$pkgname-$pkgver"
	install -Dm0644 snapper-check.desktop $pkgdir/etc/xdg/autostart/snapper-check.desktop
}
