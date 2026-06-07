import os
import sys

# Add the package's PARENT dir (= tools/) so 'from tidesdb_perf_analyze import ...' works.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
