// gen-source-icon — emits a 1024x1024 placeholder PNG for the calabi
// desktop bundle's icon.ico generation pipeline. Stdlib-only (image,
// image/png) so it works without any extra Go modules.
//
// What it draws:
//   - Rounded blue (#1677ff) square that covers the canvas
//   - White "C" silhouette in the center (an arc cut from a ring)
//
// Why pixel-by-pixel and not a real font renderer? font rendering in
// Go's stdlib alone is awful; using a TTF would pull in
// golang.org/x/image which isn't in our deps. A 2D geometric "C" is
// recognizable enough for a placeholder, and the file ships once —
// marketing replaces source.png with a real logo when ready.
//
// Usage:
//   go run ./scripts/gen-source-icon -out path/to/source.png [-size 1024]

package main

import (
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"math"
	"os"
)

func main() {
	out := flag.String("out", "source.png", "output PNG path")
	size := flag.Int("size", 1024, "image side length in pixels")
	flag.Parse()

	if *size < 32 {
		fmt.Fprintln(os.Stderr, "size must be >= 32")
		os.Exit(2)
	}

	img := image.NewNRGBA(image.Rect(0, 0, *size, *size))
	blue := color.NRGBA{R: 22, G: 119, B: 255, A: 255}
	white := color.NRGBA{R: 255, G: 255, B: 255, A: 255}
	clear := color.NRGBA{R: 0, G: 0, B: 0, A: 0}

	radius := *size * 18 / 100   // rounded-corner radius
	rOuter := *size * 36 / 100   // C outer radius
	rInner := *size * 23 / 100   // C inner radius
	centerX := *size / 2
	centerY := *size / 2

	for y := 0; y < *size; y++ {
		for x := 0; x < *size; x++ {
			// Transparent outside the rounded square.
			if !inRoundedSquare(x, y, *size, radius) {
				img.Set(x, y, clear)
				continue
			}
			// Background blue by default.
			img.Set(x, y, blue)

			// White ring (the "C") with the right side cut out.
			dx := x - centerX
			dy := y - centerY
			dist2 := dx*dx + dy*dy
			if dist2 >= rInner*rInner && dist2 <= rOuter*rOuter {
				// Angle (radians); skip the wedge facing +X (the
				// opening of the C).
				ang := math.Atan2(float64(dy), float64(dx))
				if math.Abs(ang) > math.Pi/4 {
					img.Set(x, y, white)
				}
			}
		}
	}

	f, err := os.Create(*out)
	if err != nil {
		fmt.Fprintln(os.Stderr, "create:", err)
		os.Exit(1)
	}
	defer f.Close()
	if err := png.Encode(f, img); err != nil {
		fmt.Fprintln(os.Stderr, "encode:", err)
		os.Exit(1)
	}
	fmt.Printf("wrote %s (%dx%d)\n", *out, *size, *size)
}

// inRoundedSquare returns true if (x,y) lies inside a size×size square
// with quarter-circle corners of the given radius. Used to mask the
// transparent area outside the icon's rounded shape.
func inRoundedSquare(x, y, size, radius int) bool {
	if x < radius && y < radius {
		dx := radius - x
		dy := radius - y
		return dx*dx+dy*dy < radius*radius
	}
	if x >= size-radius && y < radius {
		dx := x - (size - radius - 1)
		dy := radius - y
		return dx*dx+dy*dy < radius*radius
	}
	if x < radius && y >= size-radius {
		dx := radius - x
		dy := y - (size - radius - 1)
		return dx*dx+dy*dy < radius*radius
	}
	if x >= size-radius && y >= size-radius {
		dx := x - (size - radius - 1)
		dy := y - (size - radius - 1)
		return dx*dx+dy*dy < radius*radius
	}
	return true
}
