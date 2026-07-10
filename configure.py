import sys

try:
  from ambuild2 import run
except ImportError:
  sys.stderr.write('AMBuild 2.2 or newer is required.\n')
  sys.exit(1)

parser = run.BuildParser(sourcePath=sys.path[0], api='2.2')
parser.options.add_argument('--hl2sdk-root', default='../references')
parser.options.add_argument('--hl2sdk-manifests', default='../references/metamod-source/hl2sdk-manifests')
parser.options.add_argument('--mms-path', default='../references/metamod-source')
parser.options.add_argument('--enable-debug', action='store_true', dest='debug')
parser.Configure()
