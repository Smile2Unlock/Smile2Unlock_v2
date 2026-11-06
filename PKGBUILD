pkgname=smile2unlock
pkgver=2.4.1
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
source=('smile2unlock_2.4.1.orig.tar.gz' 'prepare-payload.py' 'smile2unlock-remove.hook')
sha256sums=('283f153b72495743419c49bc8858e09058d286547323676613b8a0a34a6e151f' 'fae7079b0bfd85cdbf5e85a97a194472ea682a76f835dc0ea40aa4d285f482cd' '2d3e6188baa0404c6c4a5156bf26de869296a43c2d91dfba3848f97dc1f59aa3')

package() {
    python "$srcdir/prepare-payload.py" "$srcdir/smile2unlock-$pkgver" "$pkgdir" /usr/lib/security --remove-hook "$srcdir/smile2unlock-remove.hook"
}
