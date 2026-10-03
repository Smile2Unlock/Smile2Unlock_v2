pkgname=smile2unlock
pkgver=2.3.3
pkgrel=1
pkgdesc='Local face authentication (upstream release binaries)'
arch=('x86_64')
url='https://github.com/Smile2Unlock/Smile2Unlock_v2'
license=('MIT')
depends=('glibc>=2.38' 'pam' 'systemd-libs' 'dbus' 'polkit')
makedepends=('python')
optdepends=('sudo: command-line administrator authentication')
conflicts=('smile2unlock-bin' 'smile2unlock-git')
options=('!strip' '!debug' '!lto')
install=smile2unlock.install
source=('smile2unlock_2.3.3.orig.tar.gz' 'prepare-payload.py' 'smile2unlock-remove.hook')
sha256sums=('86f0ea244dfe50c8b2290a085d878a4165a546399e7bba32394f21b8559d1777' 'fae7079b0bfd85cdbf5e85a97a194472ea682a76f835dc0ea40aa4d285f482cd' '88cf60bd10fc6483a35fe14e150ed531f63534696dfda021ead7562af3e4107a')

package() {
    python "$srcdir/prepare-payload.py" "$srcdir/smile2unlock-$pkgver" "$pkgdir" /usr/lib/security --remove-hook "$srcdir/smile2unlock-remove.hook"
}
