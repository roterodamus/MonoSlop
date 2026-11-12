//! peripage-clip: print the Wayland clipboard (text or image) on a Peripage A6
//! (203 dpi, 384 px wide) over Bluetooth RFCOMM.
//!
//! Protocol notes (from the reverse-engineering work in
//! github.com/bitrate16/peripage-python and eliasweingaertner/peripage-A6-bluetooth):
//!   reset          10 ff fe 01 + 12 zero bytes
//!   concentration  10 ff 10 00 <0..2>
//!   raster         1d 76 30 00 <bytes_per_row> 00 <rows> 00 + rows * bytes_per_row (1 = black)
//!   feed           1b 4a <n>

use ab_glyph::{point, Font, FontVec, PxScale, ScaleFont};
use anyhow::{anyhow, bail, Context, Result};
use clap::Parser;
use image::{imageops::FilterType, DynamicImage, GrayImage, Luma};
use serde::Deserialize;
use std::{
    fs::File,
    io::Write,
    os::fd::FromRawFd,
    path::{Path, PathBuf},
    process::Command,
    thread::sleep,
    time::Duration,
};

// ───────────────────────────── config ─────────────────────────────

#[derive(Deserialize, Debug, Clone)]
#[serde(default)]
struct Config {
    /// Bluetooth MAC address of the printer, e.g. "00:15:83:15:BC:5F".
    mac: String,
    /// RFCOMM channel (the A6 uses 1).
    channel: u8,
    /// Printable width in pixels (A6 203dpi = 384, A6+ = 576). Must be a multiple of 8.
    width: u32,
    /// Print darkness: 0 (light), 1 (medium), 2 (dark).
    concentration: u8,
    /// Paper feed after the job (0-255), so you can tear the page off.
    feed: u8,
    /// Delay between rows, ms (the printer has a tiny buffer; 10 is the known-good value).
    row_delay_ms: u64,
    /// Max rows per raster command (protocol limit is 255).
    chunk_height: u32,
    /// Pause between chunks, ms (lets the print head cool on long jobs).
    chunk_delay_ms: u64,
    /// Wait before closing the socket so the printer drains its buffer, ms.
    finish_delay_ms: u64,
    connect_timeout_secs: u64,
    connect_retries: u32,
    text: TextConfig,
    image: ImageConfig,
}

#[derive(Deserialize, Debug, Clone)]
#[serde(default)]
struct TextConfig {
    /// Font file path (contains '/') or a fontconfig name such as "monospace" or "DejaVu Sans".
    font: String,
    font_size: f32,
    /// Margin in pixels around the text.
    margin: u32,
    /// Line height multiplier.
    line_spacing: f32,
    /// Spaces per tab.
    tab_width: usize,
}

#[derive(Deserialize, Debug, Clone)]
#[serde(default)]
struct ImageConfig {
    /// Rotate landscape images (width > height) so they fill the paper width.
    rotate_landscape: bool,
    /// "cw" or "ccw".
    rotate_direction: String,
    /// "floyd-steinberg" or "threshold".
    dither: String,
    /// Threshold 0-255 (also used when dither = "threshold").
    threshold: u8,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            mac: String::new(),
            channel: 1,
            width: 384,
            concentration: 1,
            feed: 80,
            row_delay_ms: 10,
            chunk_height: 255,
            chunk_delay_ms: 0,
            finish_delay_ms: 1500,
            connect_timeout_secs: 10,
            connect_retries: 2,
            text: TextConfig::default(),
            image: ImageConfig::default(),
        }
    }
}
impl Default for TextConfig {
    fn default() -> Self {
        Self { font: "monospace".into(), font_size: 24.0, margin: 8, line_spacing: 1.1, tab_width: 4 }
    }
}
impl Default for ImageConfig {
    fn default() -> Self {
        Self {
            rotate_landscape: true,
            rotate_direction: "cw".into(),
            dither: "floyd-steinberg".into(),
            threshold: 128,
        }
    }
}

const DEFAULT_CONFIG: &str = r#"# peripage-clip configuration

# Printer Bluetooth MAC address (find it with: bluetoothctl devices)
mac = "00:00:00:00:00:00"
channel = 1            # RFCOMM channel
width = 384            # pixels: 384 for A6 (203 dpi), 576 for A6+ (300 dpi)
concentration = 1      # 0 = light, 1 = medium, 2 = dark
feed = 80              # paper feed after printing (0-255)
row_delay_ms = 10      # delay between raster rows
chunk_height = 255     # rows per raster command (max 255)
chunk_delay_ms = 0     # raise (e.g. 2000) for long, dark prints so the head can cool
finish_delay_ms = 1500 # wait before disconnecting so the printer can finish receiving
connect_timeout_secs = 10
connect_retries = 2

[text]
font = "monospace"     # fontconfig name or path to a .ttf/.otf file
font_size = 24.0       # pixels; 24 gives ~26 monospace chars per line on the A6
margin = 8
line_spacing = 1.1
tab_width = 4

[image]
rotate_landscape = true      # rotate images wider than tall
rotate_direction = "cw"      # "cw" or "ccw"
dither = "floyd-steinberg"   # or "threshold"
threshold = 128
"#;

fn default_config_path() -> PathBuf {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))
        .unwrap_or_else(|| PathBuf::from("."));
    base.join("peripage-clip").join("config.toml")
}

fn load_config(path: &Path) -> Result<Config> {
    let raw = std::fs::read_to_string(path).with_context(|| {
        format!("cannot read config {} (run `peripage-clip --init` to create one)", path.display())
    })?;
    let cfg: Config = toml::from_str(&raw).with_context(|| format!("invalid config {}", path.display()))?;
    if cfg.width == 0 || cfg.width % 8 != 0 || cfg.width / 8 > 255 {
        bail!("config: width must be a multiple of 8 and at most 2040");
    }
    if cfg.chunk_height == 0 || cfg.chunk_height > 255 {
        bail!("config: chunk_height must be 1..=255");
    }
    Ok(cfg)
}

// ───────────────────────────── CLI ─────────────────────────────

#[derive(Parser)]
#[command(version, about = "Print the Wayland clipboard (text or image) on a Peripage A6")]
struct Cli {
    /// Config file (default: ~/.config/peripage-clip/config.toml)
    #[arg(short, long)]
    config: Option<PathBuf>,
    /// Write a default config file and exit
    #[arg(long)]
    init: bool,
    /// Don't print; save a preview PNG of what would be printed
    #[arg(long, value_name = "FILE.png")]
    preview: Option<PathBuf>,
    /// Read from this file instead of the clipboard (image or UTF-8 text)
    #[arg(long, value_name = "FILE")]
    file: Option<PathBuf>,
}

// ───────────────────────────── clipboard ─────────────────────────────

enum Clip {
    Image(Vec<u8>),
    Text(String),
}

fn wl_paste(args: &[&str]) -> Result<Vec<u8>> {
    let out = Command::new("wl-paste")
        .args(args)
        .output()
        .context("failed to run `wl-paste` (install the wl-clipboard package)")?;
    if !out.status.success() {
        bail!("wl-paste: {}", String::from_utf8_lossy(&out.stderr).trim());
    }
    Ok(out.stdout)
}

fn read_clipboard() -> Result<Clip> {
    let types = String::from_utf8_lossy(&wl_paste(&["--list-types"])?).to_string();
    let types: Vec<&str> = types.lines().map(str::trim).collect();
    let supported = ["image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp"];
    // Images win over text: "Copy image" in a browser also offers text/html.
    if let Some(mime) = supported.iter().find(|m| types.contains(m)) {
        return Ok(Clip::Image(wl_paste(&["--type", mime])?));
    }
    if types.iter().any(|t| t.starts_with("text/plain") || matches!(*t, "UTF8_STRING" | "STRING" | "TEXT")) {
        let bytes = wl_paste(&["--no-newline", "--type", "text"])?;
        return Ok(Clip::Text(String::from_utf8_lossy(&bytes).into_owned()));
    }
    bail!("clipboard has no printable text or image (types: {})", types.join(", "))
}

// ───────────────────────────── rendering ─────────────────────────────

fn resolve_font(spec: &str) -> Result<FontVec> {
    let path = if spec.contains('/') {
        PathBuf::from(spec)
    } else {
        let out = Command::new("fc-match")
            .args(["-f", "%{file}", spec])
            .output()
            .context("failed to run `fc-match`; set text.font to a .ttf file path instead")?;
        PathBuf::from(String::from_utf8_lossy(&out.stdout).trim().to_string())
    };
    let data = std::fs::read(&path).with_context(|| format!("cannot read font {}", path.display()))?;
    FontVec::try_from_vec(data).map_err(|e| anyhow!("cannot parse font {}: {e}", path.display()))
}

/// Render text to a grayscale bitmap of exactly `width` pixels, word-wrapped.
fn render_text(text: &str, cfg: &Config) -> Result<GrayImage> {
    let t = &cfg.text;
    let font = resolve_font(&t.font)?;
    let scaled = font.as_scaled(PxScale::from(t.font_size));
    let avail = cfg.width.saturating_sub(2 * t.margin) as f32;
    if avail < t.font_size {
        bail!("config: margin too large for width");
    }

    // 1. Wrap into lines.
    let tab = " ".repeat(t.tab_width);
    let mut lines: Vec<String> = Vec::new();
    for raw in text.replace("\r\n", "\n").replace('\r', "\n").replace('\t', &tab).split('\n') {
        let mut line = String::new();
        let mut w = 0.0f32;
        let mut last_space: Option<usize> = None; // byte index in `line`
        let mut prev = None;
        for c in raw.chars().filter(|c| !c.is_control()) {
            let id = scaled.glyph_id(c);
            let adv = scaled.h_advance(id) + prev.map_or(0.0, |p| scaled.kern(p, id));
            if w + adv > avail && !line.is_empty() {
                if let (true, Some(i)) = (c != ' ', last_space) {
                    // break at the last space, carry the rest over
                    let rest = line[i + 1..].to_string();
                    line.truncate(i);
                    lines.push(std::mem::take(&mut line));
                    line = rest;
                } else {
                    lines.push(std::mem::take(&mut line));
                }
                last_space = None;
                prev = None;
                w = line.chars().map(|ch| scaled.h_advance(scaled.glyph_id(ch))).sum();
                if c == ' ' && line.is_empty() {
                    continue; // drop the space that caused the wrap
                }
            }
            if c == ' ' {
                last_space = Some(line.len());
            }
            let adv = scaled.h_advance(id) + prev.map_or(0.0, |p| scaled.kern(p, id));
            w += adv;
            line.push(c);
            prev = Some(id);
        }
        lines.push(line);
    }
    while lines.last().is_some_and(|l| l.trim().is_empty()) {
        lines.pop();
    }
    if lines.is_empty() {
        bail!("clipboard text is empty");
    }

    // 2. Draw.
    let line_h = ((scaled.ascent() - scaled.descent() + scaled.line_gap()) * t.line_spacing).ceil();
    let height = (lines.len() as f32 * line_h).ceil() as u32 + 2 * t.margin;
    let mut img = GrayImage::from_pixel(cfg.width, height, Luma([255]));
    for (n, line) in lines.iter().enumerate() {
        let baseline = t.margin as f32 + n as f32 * line_h + scaled.ascent();
        let mut x = t.margin as f32;
        let mut prev = None;
        for c in line.chars() {
            let id = scaled.glyph_id(c);
            if let Some(p) = prev {
                x += scaled.kern(p, id);
            }
            let glyph = id.with_scale_and_position(PxScale::from(t.font_size), point(x, baseline));
            x += scaled.h_advance(id);
            prev = Some(id);
            if let Some(o) = font.outline_glyph(glyph) {
                let b = o.px_bounds();
                o.draw(|gx, gy, cov| {
                    let px = b.min.x as i32 + gx as i32;
                    let py = b.min.y as i32 + gy as i32;
                    if px >= 0 && py >= 0 && (px as u32) < img.width() && (py as u32) < img.height() {
                        let p = img.get_pixel_mut(px as u32, py as u32);
                        p.0[0] = (p.0[0] as f32 * (1.0 - cov)).round() as u8;
                    }
                });
            }
        }
    }
    Ok(img)
}

/// Prepare a picture: flatten alpha onto white, rotate landscape, scale to paper width.
fn prepare_image(data: &[u8], cfg: &Config) -> Result<GrayImage> {
    let mut img = image::load_from_memory(data).context("cannot decode clipboard image")?;
    if cfg.image.rotate_landscape && img.width() > img.height() {
        img = if cfg.image.rotate_direction.eq_ignore_ascii_case("ccw") { img.rotate270() } else { img.rotate90() };
    }
    // Composite onto white so transparent pixels don't print black.
    let rgba = img.to_rgba8();
    let mut flat = image::RgbImage::new(rgba.width(), rgba.height());
    for (x, y, p) in rgba.enumerate_pixels() {
        let a = p[3] as f32 / 255.0;
        let mix = |c: u8| (c as f32 * a + 255.0 * (1.0 - a)).round() as u8;
        flat.put_pixel(x, y, image::Rgb([mix(p[0]), mix(p[1]), mix(p[2])]));
    }
    let gray = DynamicImage::ImageRgb8(flat).to_luma8();
    let new_h = ((gray.height() as f64) * cfg.width as f64 / gray.width() as f64).round().max(1.0) as u32;
    Ok(image::imageops::resize(&gray, cfg.width, new_h, FilterType::Lanczos3))
}

/// Convert gray to 1-bit. Returns row-major bools, `true` = black.
fn binarize(img: &GrayImage, dither: &str, threshold: u8) -> Vec<bool> {
    let (w, h) = (img.width() as usize, img.height() as usize);
    if !dither.eq_ignore_ascii_case("floyd-steinberg") {
        return img.pixels().map(|p| p[0] < threshold).collect();
    }
    let mut buf: Vec<f32> = img.pixels().map(|p| p[0] as f32).collect();
    let mut out = vec![false; w * h];
    for y in 0..h {
        for x in 0..w {
            let old = buf[y * w + x];
            let black = old < threshold as f32;
            out[y * w + x] = black;
            let err = old - if black { 0.0 } else { 255.0 };
            let mut spread = |dx: isize, dy: usize, k: f32| {
                let nx = x as isize + dx;
                if nx >= 0 && (nx as usize) < w && y + dy < h {
                    buf[(y + dy) * w + nx as usize] += err * k;
                }
            };
            spread(1, 0, 7.0 / 16.0);
            spread(-1, 1, 3.0 / 16.0);
            spread(0, 1, 5.0 / 16.0);
            spread(1, 1, 1.0 / 16.0);
        }
    }
    out
}

/// Pack to bytes, MSB first, 1 = black.
fn pack_rows(bits: &[bool], width: usize) -> Vec<Vec<u8>> {
    bits.chunks(width)
        .map(|row| {
            row.chunks(8)
                .map(|b| b.iter().enumerate().fold(0u8, |acc, (i, &on)| acc | ((on as u8) << (7 - i))))
                .collect()
        })
        .collect()
}

// ───────────────────────────── bluetooth (RFCOMM via libc) ─────────────────────────────

const BTPROTO_RFCOMM: libc::c_int = 3;

#[repr(C)]
struct SockaddrRc {
    rc_family: libc::sa_family_t,
    rc_bdaddr: [u8; 6], // little-endian (reversed) order
    rc_channel: u8,
}

fn parse_mac(s: &str) -> Result<[u8; 6]> {
    let parts: Vec<&str> = s.trim().split(':').collect();
    if parts.len() != 6 {
        bail!("invalid MAC address {s:?}; expected AA:BB:CC:DD:EE:FF");
    }
    let mut out = [0u8; 6];
    for (i, p) in parts.iter().enumerate() {
        out[5 - i] = u8::from_str_radix(p, 16).with_context(|| format!("invalid MAC address {s:?}"))?;
    }
    Ok(out)
}

fn connect_once(addr: &SockaddrRc, timeout: Duration) -> Result<File> {
    unsafe {
        let fd = libc::socket(libc::AF_BLUETOOTH, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, BTPROTO_RFCOMM);
        if fd < 0 {
            return Err(std::io::Error::last_os_error()).context("cannot create Bluetooth socket");
        }
        let file = File::from_raw_fd(fd); // closes the fd on every error path below

        let flags = libc::fcntl(fd, libc::F_GETFL);
        libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK);
        let r = libc::connect(fd, addr as *const _ as *const libc::sockaddr, std::mem::size_of::<SockaddrRc>() as u32);
        if r < 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() != Some(libc::EINPROGRESS) {
                return Err(err).context("connect failed");
            }
            let mut pfd = libc::pollfd { fd, events: libc::POLLOUT, revents: 0 };
            let n = libc::poll(&mut pfd, 1, timeout.as_millis() as i32);
            if n == 0 {
                bail!("connect timed out (is the printer on and in range?)");
            }
            if n < 0 {
                return Err(std::io::Error::last_os_error()).context("poll failed");
            }
            let mut soerr: libc::c_int = 0;
            let mut len = std::mem::size_of::<libc::c_int>() as u32;
            libc::getsockopt(fd, libc::SOL_SOCKET, libc::SO_ERROR, &mut soerr as *mut _ as *mut _, &mut len);
            if soerr != 0 {
                return Err(std::io::Error::from_raw_os_error(soerr)).context("connect failed");
            }
        }
        libc::fcntl(fd, libc::F_SETFL, flags); // back to blocking for writes
        Ok(file)
    }
}

fn connect(cfg: &Config) -> Result<File> {
    if cfg.mac.is_empty() || cfg.mac == "00:00:00:00:00:00" {
        bail!("set `mac` in the config file");
    }
    let addr = SockaddrRc {
        rc_family: libc::AF_BLUETOOTH as libc::sa_family_t,
        rc_bdaddr: parse_mac(&cfg.mac)?,
        rc_channel: cfg.channel,
    };
    let mut last = None;
    for attempt in 0..=cfg.connect_retries {
        match connect_once(&addr, Duration::from_secs(cfg.connect_timeout_secs)) {
            Ok(f) => return Ok(f),
            Err(e) => {
                eprintln!("connect attempt {} failed: {e:#}", attempt + 1);
                last = Some(e);
                sleep(Duration::from_millis(500));
            }
        }
    }
    Err(last.unwrap()).with_context(|| format!("could not connect to {}", cfg.mac))
}

// ───────────────────────────── printing ─────────────────────────────

fn reset_cmd() -> Vec<u8> {
    let mut v = vec![0x10, 0xff, 0xfe, 0x01];
    v.extend([0u8; 12]);
    v
}

fn print_rows(sock: &mut File, rows: &[Vec<u8>], cfg: &Config) -> Result<()> {
    let bytes_per_row = (cfg.width / 8) as u8;
    sock.write_all(&reset_cmd())?;
    sleep(Duration::from_millis(200));
    sock.write_all(&[0x10, 0xff, 0x10, 0x00, cfg.concentration.min(2)])?;

    let chunks: Vec<&[Vec<u8>]> = rows.chunks(cfg.chunk_height as usize).collect();
    for (i, chunk) in chunks.iter().enumerate() {
        sock.write_all(&reset_cmd())?;
        sock.write_all(&[0x1d, 0x76, 0x30, 0x00, bytes_per_row, 0x00, chunk.len() as u8, 0x00])?;
        for row in chunk.iter() {
            sock.write_all(row)?;
            sleep(Duration::from_millis(cfg.row_delay_ms));
        }
        if i + 1 < chunks.len() {
            sleep(Duration::from_millis(cfg.chunk_delay_ms));
        }
    }
    if cfg.feed > 0 {
        sock.write_all(&[0x1b, 0x4a, cfg.feed])?;
    }
    sock.flush()?;
    sleep(Duration::from_millis(cfg.finish_delay_ms));
    Ok(())
}

fn save_preview(rows: &[Vec<u8>], width: u32, path: &Path) -> Result<()> {
    let mut img = GrayImage::from_pixel(width, rows.len() as u32, Luma([255]));
    for (y, row) in rows.iter().enumerate() {
        for x in 0..width as usize {
            if row[x / 8] & (0x80 >> (x % 8)) != 0 {
                img.put_pixel(x as u32, y as u32, Luma([0]));
            }
        }
    }
    img.save(path).with_context(|| format!("cannot write {}", path.display()))
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let cfg_path = cli.config.unwrap_or_else(default_config_path);

    if cli.init {
        if cfg_path.exists() {
            bail!("{} already exists", cfg_path.display());
        }
        std::fs::create_dir_all(cfg_path.parent().unwrap_or(Path::new(".")))?;
        std::fs::write(&cfg_path, DEFAULT_CONFIG)?;
        println!("Wrote {}; edit it and set your printer's MAC address.", cfg_path.display());
        return Ok(());
    }

    let cfg = load_config(&cfg_path)?;
    let clip = match &cli.file {
        Some(p) => {
            let data = std::fs::read(p).with_context(|| format!("cannot read {}", p.display()))?;
            if image::guess_format(&data).is_ok() {
                Clip::Image(data)
            } else {
                Clip::Text(String::from_utf8_lossy(&data).into_owned())
            }
        }
        None => read_clipboard()?,
    };

    let (gray, is_text) = match &clip {
        Clip::Image(data) => (prepare_image(data, &cfg)?, false),
        Clip::Text(t) => (render_text(t, &cfg)?, true),
    };
    // Text is already high-contrast; plain thresholding keeps glyph edges crisp.
    let dither = if is_text { "threshold" } else { cfg.image.dither.as_str() };
    let bits = binarize(&gray, dither, cfg.image.threshold);
    let rows = pack_rows(&bits, cfg.width as usize);

    if let Some(path) = cli.preview {
        save_preview(&rows, cfg.width, &path)?;
        println!("Preview saved to {} ({}x{})", path.display(), cfg.width, rows.len());
        return Ok(());
    }

    let mut sock = connect(&cfg)?;
    print_rows(&mut sock, &rows, &cfg)?;
    Ok(())
}
