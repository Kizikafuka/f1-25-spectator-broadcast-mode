import os
d = os.path.dirname(os.path.abspath(__file__))
html = open(os.path.join(d, "..", "web", "index.html"), encoding="utf-8").read()
open(os.path.join(d, "..", "native", "src", "broadcast_page.hpp"), "w", encoding="utf-8").write(
    "// Generated from web/index.html by tools/embed_page.py: do not edit.\n#pragma once\nstatic const char *BP_PAGE = R\"HTML(" + html + ")HTML\";\n")
