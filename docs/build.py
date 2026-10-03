"""Assemble the manual from manual.src.html + panel_geometry.js.

  docs/manual.html  the private build (a claude.ai artifact; no <html>/<head>,
                    the host wraps it): @public blocks dropped
  docs/site.html    the public page (timncox.github.io/alchemy-lab/seq/):
                    a full document, @private blocks dropped

  <!--@private-->...<!--@/private-->   only in manual.html
  <!--@public-->...<!--@/public-->     only in site.html
"""
import pathlib
import re

d = pathlib.Path(__file__).parent
src = (d / "manual.src.html").read_text()
src = src.replace("/*GEOM*/", (d / "panel_geometry.js").read_text(), 1)


def drop(text, tag):
    return re.sub(r"<!--@%s-->.*?<!--@/%s-->" % (tag, tag), "", text, flags=re.S)


def keep(text, tag):
    return text.replace("<!--@%s-->" % tag, "").replace("<!--@/%s-->" % tag, "")


(d / "manual.html").write_text(keep(drop(src, "public"), "private"))

pub = keep(drop(src, "private"), "public")
head, body = pub.split("</style>", 1)
(d / "site.html").write_text(
    "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<meta name=\"description\" content=\"Operation manual for Seq, a generative sequencer for the "
    "Hermetic Modular Alchemy Lab: two generated voices that follow a chord progression, four drum "
    "triggers, played from the panel, a Launchpad, a Launch Control XL or a Haute42.\">\n"
    + head + "</style>\n</head>\n<body>\n" + body + "\n</body>\n</html>\n")
