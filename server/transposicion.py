"""
Motor de transposición de acordes.
Decide automáticamente entre bemoles y sostenidos según la tonalidad destino.
"""
import re
import copy

NOTAS_BEMOL =     ['C', 'Db', 'D', 'Eb', 'E', 'F', 'Gb', 'G', 'Ab', 'A', 'Bb', 'B']
NOTAS_SOSTENIDO = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']

TONOS_CON_SOSTENIDOS = {'G', 'D', 'A', 'E', 'B',
                        'Gm', 'Dm', 'Am', 'Em', 'Bm', 'F#m', 'C#m', 'G#m', 'D#m', 'A#m'}

NOTA_A_INDICE = {
    'C': 0, 'B#': 0, 'C#': 1, 'Db': 1, 'D': 2, 'D#': 3, 'Eb': 3,
    'E': 4, 'Fb': 4, 'F': 5, 'E#': 5, 'F#': 6, 'Gb': 6, 'G': 7,
    'G#': 8, 'Ab': 8, 'A': 9, 'A#': 10, 'Bb': 10, 'B': 11, 'Cb': 11,
}

PATRON_ACORDE = re.compile(r'^([A-G][#b]?)([^/\s]*)(?:/([A-G][#b]?))?$')


def usar_sostenidos(tono_destino):
    if not tono_destino:
        return False
    match = re.match(r'^([A-G][#b]?)(m?)', tono_destino.strip())
    if not match:
        return False
    raiz, menor = match.groups()
    return (raiz + (menor or '')) in TONOS_CON_SOSTENIDOS


def transponer_nota(nota, semitonos, usar_sost=False):
    if nota not in NOTA_A_INDICE:
        return nota
    nuevo = (NOTA_A_INDICE[nota] + semitonos) % 12
    return (NOTAS_SOSTENIDO if usar_sost else NOTAS_BEMOL)[nuevo]


def transponer_acorde(acorde, semitonos, usar_sost=False):
    acorde = acorde.strip()
    if not acorde:
        return acorde
    match = PATRON_ACORDE.match(acorde)
    if not match:
        return acorde
    raiz, modificador, bajo = match.groups()
    resultado = transponer_nota(raiz, semitonos, usar_sost) + (modificador or '')
    if bajo:
        resultado += '/' + transponer_nota(bajo, semitonos, usar_sost)
    return resultado


def transponer_texto_acordes(texto, semitonos, usar_sost=False):
    if not texto or not texto.strip():
        return texto
    partes = re.split(r'(\s+)', texto)
    return ''.join(
        p if not p.strip() else transponer_acorde(p, semitonos, usar_sost)
        for p in partes
    )


def transponer_cancion(cancion, semitonos):
    """Transponer una canción completa N semitonos.
    Decide automáticamente bemoles vs sostenidos según el tono destino."""
    if semitonos == 0:
        return cancion
    
    nueva = copy.deepcopy(cancion)
    tono_original = nueva.get('tono', 'C')
    
    # Decidir bemoles o sostenidos según cómo se ve más natural el tono destino
    tono_sost = transponer_acorde(tono_original, semitonos, usar_sost=True)
    usar_sost = usar_sostenidos(tono_sost)
    
    nueva['tono'] = transponer_acorde(tono_original, semitonos, usar_sost)
    
    for sec in nueva.get('secciones', []):
        if 'prog' in sec:
            sec['prog'] = [transponer_texto_acordes(p, semitonos, usar_sost) for p in sec['prog']]
        if 'lines' in sec:
            for line in sec['lines']:
                for tok in line:
                    if len(tok) >= 1 and tok[0]:
                        tok[0] = transponer_texto_acordes(tok[0], semitonos, usar_sost)
    return nueva
