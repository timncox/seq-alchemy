"""Assemble docs/manual.html: manual.src.html with panel_geometry.js inlined.
The page is published as a claude.ai artifact (no <html>/<head>: the host wraps it)."""
import pathlib
d = pathlib.Path(__file__).parent
src = (d / "manual.src.html").read_text()
geom = (d / "panel_geometry.js").read_text()
(d / "manual.html").write_text(src.replace("/*GEOM*/", geom, 1))
