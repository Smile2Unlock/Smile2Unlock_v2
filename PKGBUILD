pkgname=smile2unlock
pkgver=2.4.0
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
source=('smile2unlock_2.4.0.orig.tar.gz' 'prepare-payload.py' 'smile2unlock-remove.hook')
sha256sums=('8a739cc128187cdda1e85cab938b2e9338e9dd5916fd85b2c5148d73b52b55b6' 'fae7079b0bfd85cdbf5e85a97a194472ea682a76f835dc0ea40aa4d285f482cd' '88cf60bd10fc6483a35fe14e150ed531f63534696dfda021ead7562af3e4107a')

package() {
    python "$srcdir/prepare-payload.py" "$srcdir/smile2unlock-$pkgver" "$pkgdir" /usr/lib/security --remove-hook "$srcdir/smile2unlock-remove.hook"
}
