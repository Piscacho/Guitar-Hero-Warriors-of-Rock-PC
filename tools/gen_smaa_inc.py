"""Genera runtime/smaa/smaa_hlsl.inc: el SMAA.hlsl oficial (iryoku/smaa, licencia MIT) como cadena de C++ para compilarlo en tiempo
de ejecucion con D3DCompile. Se trocea en partes de ~12 KB (limite de longitud de los literales de cadena). Uso: python tools/gen_smaa_inc.py"""
import io, os
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'runtime', 'smaa')
src = io.open(os.path.join(R, 'SMAA.hlsl'), encoding='latin-1', newline='').read()
src = ''.join(c if ord(c) < 128 else '?' for c in src)   # solo hay caracteres no ASCII en comentarios
assert ')SMAA"' not in src
parts, cur = [], ''
for line in src.splitlines(keepends=True):
    if len(cur) + len(line) > 12000:
        parts.append(cur); cur = ''
    cur += line
parts.append(cur)
out = ['// GENERADO por tools/gen_smaa_inc.py a partir de SMAA.hlsl (Jorge Jimenez et al., licencia MIT, ver LICENSE.txt). No editar.',
       'static const char* const kSmaaHlslParts[] = {']
out += ['R"SMAA(' + p + ')SMAA",' for p in parts]
out += ['};', '']
io.open(os.path.join(R, 'smaa_hlsl.inc'), 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
print('partes:', len(parts))
