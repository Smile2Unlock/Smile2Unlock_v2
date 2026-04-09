from pathlib import Path
import sys

try:
    from PIL import Image, ImageFilter
except ImportError as exc:
    raise SystemExit(
        "Pillow is required to generate brand assets. Install it with: python -m pip install pillow"
    ) from exc


ROOT = Path(__file__).resolve().parents[1]
SOURCE_PNG = ROOT / "common" / "resources" / "img" / "Smile2Unlock.png"
CP_TILE_BMP = ROOT / "CredentialProvider" / "tileimage.bmp"
APP_ICON_ICO = ROOT / "common" / "resources" / "img" / "Smile2Unlock.ico"
APP_PANEL_BMP = ROOT / "common" / "resources" / "img" / "Smile2Unlock_panel.bmp"


def ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def fit_square(image: Image.Image, size: int, padding: int) -> Image.Image:
    inner_size = max(1, size - padding * 2)
    fitted = image.copy()
    fitted.thumbnail((inner_size, inner_size), Image.Resampling.LANCZOS)

    canvas = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    offset = ((size - fitted.width) // 2, (size - fitted.height) // 2)
    canvas.alpha_composite(fitted, offset)
    return canvas


def render_cp_tile(image: Image.Image) -> Image.Image:
    size = 128
    background = Image.new("RGBA", (size, size), (235, 244, 250, 255))
    logo = fit_square(image, size, padding=10)
    logo = logo.filter(ImageFilter.UnsharpMask(radius=1.4, percent=135, threshold=2))
    background.alpha_composite(logo)
    return background.convert("RGB")


def render_panel_bmp(image: Image.Image) -> Image.Image:
    panel = fit_square(image, 256, padding=12)
    panel = panel.filter(ImageFilter.UnsharpMask(radius=1.0, percent=120, threshold=2))
    return panel.convert("RGB")


def render_icon(image: Image.Image) -> Image.Image:
    icon = fit_square(image, 256, padding=8)
    return icon.convert("RGBA")


def main() -> int:
    if not SOURCE_PNG.is_file():
        print(f"Brand source not found: {SOURCE_PNG}", file=sys.stderr)
        return 1

    source = Image.open(SOURCE_PNG).convert("RGBA")

    ensure_parent(CP_TILE_BMP)
    ensure_parent(APP_ICON_ICO)
    ensure_parent(APP_PANEL_BMP)

    render_cp_tile(source).save(CP_TILE_BMP, format="BMP")
    render_panel_bmp(source).save(APP_PANEL_BMP, format="BMP")
    render_icon(source).save(
        APP_ICON_ICO,
        format="ICO",
        sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)],
    )

    print(f"Generated {CP_TILE_BMP.relative_to(ROOT)}")
    print(f"Generated {APP_PANEL_BMP.relative_to(ROOT)}")
    print(f"Generated {APP_ICON_ICO.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
