#!/usr/bin/env bash
# harness: xcursor-theme=true
# The SVG cursor backend renders a theme directory of static SVGs through
# Cairo. The buffer is supersampled and keeps the viewBox aspect ratio, and a
# name the theme does not provide falls back to the Xcursor manager.
set -euo pipefail

THEME=$UMBRIEL_RUNTIME_DIR/svg-cursors
SCALABLE=$UMBRIEL_RUNTIME_DIR/scalable-cursors
EMPTY=$UMBRIEL_RUNTIME_DIR/empty-cursors
POINTER=${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}
mkdir -p "$THEME" "$SCALABLE/cursors_scalable/left_ptr" "$EMPTY"

cat > "$THEME/theme.toml" <<'EOF'
[theme]
hotspot_x = 0.5
hotspot_y = 0.5

[cursor.default]
hotspot_x = 0.25
hotspot_y = 0.75
frames = ["default/f0.svg", "default/f1.svg"]
delays = [60, 60]
EOF

# 48x24 viewBox: twice as wide as tall, supersampled 4x into a 192x96 buffer.
cat > "$THEME/default.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 24" width="48" height="24">
  <rect x="1" y="1" width="46" height="22" rx="4" fill="#112233" stroke="#ffffff" stroke-width="2"/>
</svg>
EOF

mkdir -p "$THEME/default"
# Frame 1 is portrait: the animation swaps to a 96x192 texture at 4x.
cat > "$THEME/default/f0.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 24" width="48" height="24">
  <rect x="1" y="1" width="46" height="22" rx="4" fill="#112233" stroke="#ffffff" stroke-width="2"/>
</svg>
EOF
cat > "$THEME/default/f1.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 48" width="24" height="48">
  <rect x="1" y="1" width="22" height="46" rx="4" fill="#332211" stroke="#ffffff" stroke-width="2"/>
</svg>
EOF

# A minimal KDE/GNOME cursors_scalable theme: per-cursor directory with an
# SVG and metadata.json carrying the hotspot in nominal-size pixels.
cat > "$SCALABLE/cursors_scalable/left_ptr/left_ptr.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32">
  <path d="M4 4 L28 16 L4 28 Z" fill="#00ff00" stroke="#000000" stroke-width="2"/>
</svg>
EOF
cat > "$SCALABLE/cursors_scalable/left_ptr/metadata.json" <<'EOF'
[{"filename": "left_ptr.svg", "hotspot_x": 4, "hotspot_y": 4, "nominal_size": 32}]
EOF

point_theme() {
  sed -i "s|^theme = \".*\"|theme = \"$1\"|" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

cursor_state() {
  "$UMBRIEL" cursor-state --json 2>/dev/null || true
}

await_image() {
  local label=$1 expression=$2 state=
  for _ in $(seq 80); do
    state=$(cursor_state)
    if jq -e "$expression" <<< "$state" > /dev/null 2>&1; then
      return 0
    fi
    sleep 0.05
  done
  echo "$label did not reach the expected cursor state: ${state:-no response}"
  return 1
}

# Switch [input.cursor] to the SVG backend. The harness config already carries
# theme/size under [input.cursor] because xcursor-theme=true.
sed -i '/^\[input.cursor\]$/a backend = "svg"' "$UMBRIEL_CONFIG"
point_theme "$THEME"

"$POINTER" 1280 720 move 640 360 > /dev/null
await_image "svg cursor" '.image.texture_width == 192 and .image.texture_height == 96 and .image.render_width == 48 and .image.render_height == 24'

# The two-frame default cursor cycles: the portrait second frame renders as a
# 96x192 texture. Polling catches whichever frame is up.
await_image "svg animation frame" '.image.texture_width == 96 and .image.texture_height == 192'

# A theme directory without the requested name resolves through the Xcursor
# manager instead. Which image that produces depends on the installed cursor
# themes, so assert only that the SVG buffer is gone.
point_theme "$EMPTY"
"$POINTER" 1280 720 move 641 360 > /dev/null
await_image "xcursor fallback" '.image != null and (.image.texture_width != 192 or .image.texture_height != 96)'

# A cursors_scalable layout theme renders through the same backend: the 32x32
# viewBox supersamples to a 128x128 texture.
point_theme "$SCALABLE"
"$POINTER" 1280 720 move 642 360 > /dev/null
await_image "scalable cursor" '.image.texture_width == 128 and .image.texture_height == 128'

echo "svg cursor backend renders aspect-preserved buffers, frame animation, cursors_scalable themes, and falls back to Xcursor"
