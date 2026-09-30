# Compile hors du jeu les shaders HLSL de src/gfx9.cpp (fxc du SDK) : une erreur de syntaxe coupe tout le rendu
# moderne en jeu (le texte entier est refuse), on la voit ici avant de lancer une partie.
#   python run/check-shaders.py
import re, subprocess, sys, os, tempfile
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(root, 'src', 'gfx9.cpp'), encoding='utf-8').read()
fxc = r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe'
# kShaders : blocs R"HLSL(...)HLSL" consecutifs (chaines collees), comme le compile le jeu.
start = src.index('kShaders[] =')
m = re.compile(r'\s*R"HLSL\((.*?)\)HLSL"', re.S)
pos, parts = src.index('R"HLSL(', start), []
while True:
    r = m.match(src, pos)
    if not r: break
    parts.append(r.group(1)); pos = r.end()
blocks = [''.join(parts)]
entries = re.findall(r'CompileShader\("(\w+)", "(\w+)"', src)
tmp = os.path.join(tempfile.gettempdir(), 'vccoop-shaders.hlsl')
bad = 0
for i, text in enumerate(blocks):
    open(tmp, 'w', encoding='utf-8').write(text)
    for entry, target in sorted(set(entries)):
        if not re.search(r'\b' + entry + r'\s*\(', text): continue
        r = subprocess.run([fxc, '/nologo', '/T', target, '/E', entry, '/Fo', os.devnull, tmp], capture_output=True, text=True)
        if r.returncode:
            bad += 1
            print('ECHEC', entry, (r.stderr or r.stdout).strip().splitlines()[:3])
print('%d bloc(s), %s' % (len(blocks), 'erreurs : %d' % bad if bad else 'tout compile'))
sys.exit(1 if bad else 0)
