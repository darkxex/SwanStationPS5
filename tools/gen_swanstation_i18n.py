#!/usr/bin/env python3
"""Generates src/i18n_swanstation.inc: the French, Portuguese and Spanish text of SwanStation's core options
(their names, help and value names) and of the groups they are listed under.

    python3 tools/gen_swanstation_i18n.py

The English text comes from src/core/swanstation_options.c (run tools/gen_swanstation_options.py first when
the core's options change); the translations below are keyed by the option's libretro key. The generated
file is committed and included by src/i18n.c. Japanese keeps the English text for these entries.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def c_unescape(s):
    return re.sub(r'\\(.)', lambda m: {"n": "\n"}.get(m.group(1), m.group(1)), s)


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


# ---------------------------------------------------------------- option names and help: key -> (fr, pt, es)
LABEL = {}
HELP = {}


def opt(key, label, help_):
    LABEL[key] = label
    HELP[key] = help_


for region, tag in (("NTSCJ", "NTSC-J"), ("NTSCU", "NTSC-U"), ("PAL", "PAL")):
    opt(f"swanstation_BIOS_Path{region}",
        (f"BIOS {tag} (redémarrage)", f"BIOS {tag} (reiniciar)", f"BIOS {tag} (reiniciar)"),
        (f"Choisit la BIOS standard à utiliser pour le {tag}.", f"Escolhe a BIOS padrão a usar para {tag}.",
         f"Elige qué BIOS estándar usar para {tag}."))

opt("swanstation_CDROM_RegionCheck",
    ("Vérification de la région du CD-ROM", "Verificação da região do CD-ROM", "Verificación de región del CD-ROM"),
    ("Empêche l'émulateur de lire les disques d'une mauvaise région. Peut généralement être désactivé sans risque.",
     "Impede que o emulador leia discos de regiões incorretas. Normalmente pode ser desativado sem problemas.",
     "Evita que el emulador lea discos de regiones incorrectas. Normalmente se puede desactivar sin problema."))
opt("swanstation_CDROM_ReadThread",
    ("Thread de lecture du CD-ROM", "Thread de leitura do CD-ROM", "Hilo de lectura del CD-ROM"),
    ("Lit les secteurs du CD-ROM à l'avance de façon asynchrone, ce qui réduit les pics de temps d'image.",
     "Lê setores do CD-ROM antecipadamente de forma assíncrona, reduzindo picos no tempo de imagem.",
     "Lee sectores del CD-ROM por adelantado de forma asíncrona, lo que reduce los picos de tiempo por cuadro."))
opt("swanstation_CDROM_LoadImagePatches",
    ("Appliquer les patchs d'image (redémarrage)", "Aplicar patches de imagem (reiniciar)",
     "Aplicar parches de imagen (reiniciar)"),
    ("Applique automatiquement les patchs aux images de disque présents dans le même dossier. Seuls les patchs PPF "
     "sont pris en charge pour l'instant. Nécessite de redémarrer le cœur.",
     "Aplica automaticamente patches às imagens de disco que estejam na mesma pasta. De momento só são suportados "
     "patches PPF. Requer reiniciar o núcleo.",
     "Aplica automáticamente los parches a las imágenes de disco que estén en la misma carpeta. Por ahora solo se "
     "admiten parches PPF. Requiere reiniciar el núcleo."))
opt("swanstation_CDROM_LoadImageToRAM",
    ("Précharger l'image du CD-ROM en RAM", "Pré-carregar a imagem do CD-ROM na RAM",
     "Precargar la imagen del CD-ROM en RAM"),
    ("Charge l'image du disque en RAM avant de lancer l'émulation. Peut réduire les saccades depuis un partage "
     "réseau, au prix d'un démarrage plus long. L'émulateur semblera bloqué pendant le préchargement.",
     "Carrega a imagem do disco na RAM antes de iniciar a emulação. Pode reduzir os soluços a partir de uma partilha "
     "de rede, à custa de um arranque mais demorado. O emulador parecerá bloqueado durante o pré-carregamento.",
     "Carga la imagen del disco en RAM antes de iniciar la emulación. Puede reducir los tirones si juegas desde una "
     "carpeta de red, a costa de un inicio más lento. El emulador parecerá congelado mientras se precarga la imagen."))
opt("swanstation_CDROM_PreCacheCHD",
    ("Pré-mettre en cache les images CHD en RAM", "Pré-guardar imagens CHD na RAM", "Precachear imágenes CHD en RAM"),
    ("Met en cache les images CHD en RAM sans les décompresser. Contrairement au préchargement, elle accepte les "
     "fichiers M3U, et elle est plus rapide et demande moins de RAM.",
     "Guarda as imagens CHD na RAM sem as descomprimir. Ao contrário do pré-carregamento, suporta ficheiros M3U, é "
     "mais rápida e usa menos RAM.",
     "Guarda las imágenes CHD en RAM sin descomprimirlas. A diferencia de la precarga, admite archivos M3U, y es más "
     "rápida y usa menos RAM."))
opt("swanstation_CDROM_MuteCDAudio",
    ("Couper l'audio du CD", "Silenciar o áudio do CD", "Silenciar el audio del CD"),
    ("Coupe de force l'audio CD-DA et XA du CD-ROM. Permet de supprimer la musique de fond de certains jeux.",
     "Silencia à força o áudio CD-DA e XA do CD-ROM. Permite desativar a música de fundo em alguns jogos.",
     "Silencia a la fuerza el audio CD-DA y XA del CD-ROM. Sirve para quitar la música de fondo en algunos juegos."))
opt("swanstation_CDROM_SeekSpeedup",
    ("Accélération de la recherche du CD-ROM", "Aceleração da procura do CD-ROM",
     "Aceleración de búsqueda del CD-ROM"),
    ("Accélère les recherches du CD-ROM du facteur indiqué. Peut raccourcir les chargements de certains jeux, mais "
     "en casse d'autres.",
     "Acelera as procuras do CD-ROM pelo fator indicado. Pode melhorar os tempos de carregamento de alguns jogos, "
     "mas estraga outros.",
     "Acelera las búsquedas del CD-ROM según el factor indicado. Puede mejorar los tiempos de carga de algunos "
     "juegos, pero rompe otros."))
opt("swanstation_CPU_ExecutionMode",
    ("Mode d'exécution du CPU", "Modo de execução do CPU", "Modo de ejecución de la CPU"),
    ("Mode utilisé pour émuler le CPU. Le recompilateur offre les meilleures performances.",
     "Modo usado para emular o CPU. O recompilador dá o melhor desempenho.",
     "Modo que se usa para emular la CPU. El recompilador da el mejor rendimiento."))
opt("swanstation_GPU_UseThread",
    ("Rendu multithread (logiciel)", "Renderização com threads (software)", "Renderizado con hilos (software)"),
    ("Utilise un second thread pour dessiner. Pour l'instant, uniquement avec le rendu logiciel.",
     "Usa uma segunda thread para desenhar os gráficos. De momento só está disponível no renderizador por software.",
     "Usa un segundo hilo para dibujar los gráficos. Por ahora solo está disponible con el renderizador por "
     "software."))
opt("swanstation_GPU_UseSoftwareRendererForReadbacks",
    ("Rendu logiciel pour les lectures de VRAM (redémarrage)",
     "Renderizador por software para leituras da VRAM (reiniciar)",
     "Renderizador por software para lecturas de VRAM (reiniciar)"),
    ("Fait tourner le rendu logiciel en parallèle pour les lectures de VRAM. Certains jeux en ont besoin pour "
     "afficher correctement certains effets (flou de mouvement, spirales de combat...) ou pour corriger des "
     "textures corrompues avec le rendu matériel. Nécessite de redémarrer le cœur.",
     "Executa o renderizador por software em paralelo para as leituras da VRAM. Alguns jogos precisam disto para "
     "mostrar corretamente certos efeitos (desfocagem de movimento, espirais de combate, etc.) ou para corrigir "
     "texturas corrompidas com o renderizador por hardware. Requer reiniciar o núcleo.",
     "Ejecuta el renderizador por software en paralelo para las lecturas de VRAM. Algunos juegos lo necesitan para "
     "mostrar bien ciertos efectos (desenfoque de movimiento, espirales de batalla, etc.) o para corregir texturas "
     "dañadas con el renderizador por hardware. Requiere reiniciar el núcleo."))
opt("swanstation_GPU_MSAA",
    ("Anticrénelage multi-échantillon (MSAA)", "Antisserrilhado multiamostra (MSAA)",
     "Antialiasing multimuestra (MSAA)"),
    ("Utilise l'anticrénelage multi-échantillon pour les objets 3D. Lisse les bords dentelés des polygones pour un "
     "coût moindre qu'une résolution plus élevée, mais risque davantage de provoquer des erreurs d'affichage dans "
     "certains jeux.",
     "Usa antisserrilhado multiamostra para os objetos 3D. Suaviza as arestas dos polígonos com menor custo do que "
     "aumentar a resolução, mas é mais provável causar erros de imagem em alguns jogos.",
     "Usa antialiasing multimuestra para los objetos 3D. Suaviza los bordes dentados de los polígonos con menos "
     "costo que subir la resolución, pero puede causar más errores gráficos en algunos juegos."))
opt("swanstation_GPU_ScaledDithering",
    ("Tramage à l'échelle", "Dithering escalado", "Dithering escalado"),
    ("Adapte le motif de tramage à la résolution interne, ce qui le rend moins visible. Peut généralement être "
     "activé sans risque.",
     "Escala o padrão de dithering com a resolução interna, tornando-o menos visível. Normalmente pode ser ativado "
     "sem problemas.",
     "Escala el patrón de dithering con la resolución interna, y lo hace menos visible. Normalmente se puede "
     "activar sin problema."))
opt("swanstation_GPU_DisableInterlacing",
    ("Désactiver l'entrelacement", "Desativar entrelaçamento", "Desactivar entrelazado"),
    ("Désactive le rendu et l'affichage entrelacés du GPU. Certains jeux passent ainsi en 480p, d'autres cessent de "
     "fonctionner.",
     "Desativa o rendering e a apresentação entrelaçados no GPU. Alguns jogos passam a 480p, mas outros deixam de "
     "funcionar.",
     "Desactiva el renderizado y la imagen entrelazados de la GPU. Algunos juegos pasan a 480p, pero otros dejan de "
     "funcionar."))
opt("swanstation_GPU_ForceNTSCTimings",
    ("Forcer le timing NTSC", "Forçar temporização NTSC", "Forzar temporización NTSC"),
    ("Force les jeux PAL à tourner au timing NTSC, soit 60 Hz. Certains jeux PAL tournent alors à leur vitesse "
     "« normale », d'autres cessent de fonctionner.",
     "Força os jogos PAL a funcionar com temporização NTSC, ou seja, 60 Hz. Alguns jogos PAL ficam à velocidade "
     "«normal», outros deixam de funcionar.",
     "Obliga a los juegos PAL a funcionar con temporización NTSC, es decir, 60 Hz. Algunos juegos PAL van a su "
     "velocidad «normal», pero otros dejan de funcionar."))
opt("swanstation_Display_Force4_3For24Bit",
    ("Forcer le 4:3 pour l'affichage 24 bits", "Forçar 4:3 para imagem de 24 bits",
     "Forzar 4:3 en pantalla de 24 bits"),
    ("Repasse en format 4:3 pour le contenu 24 bits, généralement les vidéos (FMV).",
     "Volta ao formato 4:3 ao mostrar conteúdo de 24 bits, normalmente vídeos (FMV).",
     "Vuelve a la relación 4:3 al mostrar contenido de 24 bits, normalmente videos (FMV)."))
opt("swanstation_GPU_ChromaSmoothing24Bit",
    ("Lissage chromatique pour l'affichage 24 bits", "Suavização de croma para imagem de 24 bits",
     "Suavizado de croma en pantalla de 24 bits"),
    ("Atténue l'effet de blocs entre les transitions de couleur du contenu 24 bits, généralement les vidéos (FMV). "
     "Uniquement avec le rendu matériel.",
     "Atenua o efeito de blocos nas transições de cor do conteúdo de 24 bits, normalmente vídeos (FMV). Só se "
     "aplica ao renderizador por hardware.",
     "Atenúa el efecto de bloques en las transiciones de color del contenido de 24 bits, normalmente videos (FMV). "
     "Solo se aplica al renderizador por hardware."))
opt("swanstation_GPU_TextureFilter",
    ("Filtrage des textures", "Filtragem de texturas", "Filtrado de texturas"),
    ("Atténue l'aspect pixelisé des textures agrandies des objets 3D par un filtrage bilinéaire. L'effet est plus "
     "marqué à haute résolution. Uniquement avec le rendu matériel.",
     "Atenua o aspeto de blocos das texturas ampliadas dos objetos 3D com filtragem bilinear. O efeito é maior com "
     "resoluções mais altas. Só se aplica ao renderizador por hardware.",
     "Suaviza el aspecto pixelado de las texturas ampliadas de los objetos 3D con filtrado bilineal. Tiene más "
     "efecto con resoluciones altas. Solo se aplica al renderizador por hardware."))
opt("swanstation_GPU_WidescreenHack",
    ("Hack écran large", "Hack de ecrã panorâmico", "Hack de pantalla ancha"),
    ("Élargit le champ de vision de 4:3 au format choisi dans les jeux 3D. Ne fonctionne pas comme prévu avec les "
     "jeux 2D ou ceux à fonds pré-rendus.",
     "Alarga o campo de visão de 4:3 para o formato escolhido nos jogos 3D. Não funciona como esperado em jogos 2D "
     "ou com fundos pré-renderizados.",
     "Amplía el campo de visión de 4:3 a la relación elegida en los juegos 3D. No funciona como se espera en "
     "juegos 2D o con fondos prerrenderizados."))
opt("swanstation_GPU_PGXPCulling",
    ("Correction du culling PGXP", "Correção de culling PGXP", "Corrección de culling PGXP"),
    ("Augmente la précision de l'élimination des polygones et réduit les trous dans la géométrie. Nécessite la "
     "correction de géométrie.",
     "Aumenta a precisão da eliminação de polígonos e reduz os buracos na geometria. Requer a correção de geometria "
     "ativada.",
     "Aumenta la precisión del descarte de polígonos y reduce los huecos en la geometría. Requiere tener activada "
     "la corrección de geometría."))
opt("swanstation_GPU_PGXPTextureCorrection",
    ("Textures corrigées en perspective (PGXP)", "Texturas com perspetiva correta (PGXP)",
     "Texturas con perspectiva correcta (PGXP)"),
    ("Interpole les coordonnées de texture avec correction de perspective, ce qui redresse les textures déformées. "
     "Nécessite la correction de géométrie.",
     "Interpola as coordenadas das texturas com perspetiva correta, endireitando as texturas deformadas. Requer a "
     "correção de geometria ativada.",
     "Interpola las coordenadas de textura con perspectiva correcta y endereza las texturas deformadas. Requiere "
     "tener activada la corrección de geometría."))
opt("swanstation_GPU_PGXPColorCorrection",
    ("Couleurs corrigées en perspective (PGXP)", "Cores com perspetiva correta (PGXP)",
     "Colores con perspectiva correcta (PGXP)"),
    ("Interpole les couleurs des sommets avec correction de perspective. Peut améliorer l'image de certains jeux, "
     "mais provoquer des erreurs dans d'autres. Nécessite la correction de géométrie.",
     "Interpola as cores dos vértices com perspetiva correta. Pode melhorar a imagem de alguns jogos, mas causar "
     "erros noutros. Requer a correção de geometria ativada.",
     "Interpola los colores de los vértices con perspectiva correcta. Puede mejorar la imagen de algunos juegos, "
     "pero causar errores en otros. Requiere tener activada la corrección de geometría."))
opt("swanstation_GPU_PGXPDepthBuffer",
    ("Tampon de profondeur PGXP", "Buffer de profundidade PGXP", "Búfer de profundidad PGXP"),
    ("Tente de réduire le Z-fighting des polygones en testant les pixels avec les profondeurs de PGXP. "
     "Compatibilité faible, mais efficace dans certains jeux. Nécessite la correction de géométrie.",
     "Tenta reduzir o Z-fighting dos polígonos testando os pixéis com os valores de profundidade do PGXP. "
     "Compatibilidade baixa, mas funciona bem em alguns jogos. Requer a correção de geometria ativada.",
     "Intenta reducir el Z-fighting de los polígonos comprobando los píxeles con los valores de profundidad de "
     "PGXP. Compatibilidad baja, pero funciona bien en algunos juegos. Requiere tener activada la corrección de "
     "geometría."))
opt("swanstation_GPU_PGXPVertexCache",
    ("Cache de sommets PGXP", "Cache de vértices PGXP", "Caché de vértices PGXP"),
    ("Utilise les coordonnées d'écran en secours quand le suivi des sommets en mémoire échoue. Peut améliorer la "
     "compatibilité de PGXP.",
     "Usa as coordenadas do ecrã como alternativa quando falha o seguimento dos vértices na memória. Pode melhorar "
     "a compatibilidade do PGXP.",
     "Usa las coordenadas de pantalla como alternativa cuando falla el seguimiento de vértices en memoria. Puede "
     "mejorar la compatibilidad de PGXP."))
opt("swanstation_GPU_PGXPCPU",
    ("Mode CPU PGXP", "Modo CPU PGXP", "Modo CPU de PGXP"),
    ("Tente de suivre la manipulation des sommets par le CPU. Certains jeux en ont besoin pour que PGXP soit "
     "efficace. Très lent, et incompatible avec le recompilateur.",
     "Tenta seguir a manipulação dos vértices pelo CPU. Alguns jogos precisam disto para o PGXP ser eficaz. Muito "
     "lento e incompatível com o recompilador.",
     "Intenta seguir la manipulación de vértices por la CPU. Algunos juegos lo necesitan para que PGXP sea "
     "efectivo. Muy lento e incompatible con el recompilador."))
opt("swanstation_GPU_PGXPPreserveProjFP",
    ("Préserver la précision de projection PGXP", "Preservar a precisão da projeção PGXP",
     "Preservar la precisión de proyección PGXP"),
    ("Active une précision supplémentaire pour PGXP. Peut améliorer l'image de certains jeux et en casser d'autres.",
     "Ativa precisão adicional no PGXP. Pode melhorar a imagem de alguns jogos e estragar outros.",
     "Activa precisión adicional en PGXP. Puede mejorar la imagen de algunos juegos y romper otros."))
opt("swanstation_GPU_PGXPTolerance",
    ("Tolérance géométrique PGXP", "Tolerância de geometria PGXP", "Tolerancia de geometría PGXP"),
    ("Ignore les positions précises si l'écart dépasse ce seuil.",
     "Ignora as posições precisas se a diferença exceder este limite.",
     "Ignora las posiciones precisas si la diferencia supera este umbral."))
opt("swanstation_GPU_PGXPDepthClearThreshold",
    ("Seuil d'effacement de profondeur PGXP", "Limite de limpeza de profundidade PGXP",
     "Umbral de limpieza de profundidad PGXP"),
    ("Définit le seuil du tampon de profondeur PGXP.", "Define o limite do buffer de profundidade PGXP.",
     "Define el umbral del búfer de profundidad de PGXP."))
opt("swanstation_Display_AspectRatio",
    ("Format d'image fourni par le cœur", "Proporção fornecida pelo núcleo", "Relación de aspecto del núcleo"),
    ("Définit le format d'image préféré. Forcé sur « Corrected (NTSC) » avec un Namco GunCon.",
     "Define a proporção preferida. É forçada a «Corrected (NTSC)» com um Namco GunCon.",
     "Define la relación de aspecto preferida. Se fuerza a «Corrected (NTSC)» con un Namco GunCon."))
opt("swanstation_Display_CustomAspectRatioNumerator",
    ("Numérateur du format personnalisé", "Numerador da proporção personalizada",
     "Numerador de la relación personalizada"),
    ("Numérateur du format personnalisé, par exemple 4 pour 4:3.",
     "Numerador da proporção personalizada, por exemplo 4 em 4:3.",
     "Numerador de la relación personalizada, por ejemplo 4 en 4:3."))
opt("swanstation_Display_CustomAspectRatioDenominator",
    ("Dénominateur du format personnalisé", "Denominador da proporção personalizada",
     "Denominador de la relación personalizada"),
    ("Dénominateur du format personnalisé, par exemple 3 pour 4:3.",
     "Denominador da proporção personalizada, por exemplo 3 em 4:3.",
     "Denominador de la relación personalizada, por ejemplo 3 en 4:3."))
opt("swanstation_Display_CropMode",
    ("Mode de recadrage", "Modo de recorte", "Modo de recorte"),
    ("Change la part de l'image qui est rognée. Certains jeux affichent des déchets dans la zone de surbalayage, "
     "qui est normalement masquée.",
     "Altera quanto da imagem é recortado. Alguns jogos mostram lixo na área de overscan, que normalmente fica "
     "escondida.",
     "Cambia cuánto de la imagen se recorta. Algunos juegos muestran basura en el área de overscan, que "
     "normalmente queda oculta."))
opt("swanstation_GPU_DownsampleMode",
    ("Sous-échantillonnage", "Redução de amostragem", "Reducción de muestreo"),
    ("Réduit l'image rendue avant de l'afficher. Peut améliorer la qualité des jeux mêlant 2D et 3D, mais à "
     "désactiver pour les jeux purement 3D. Uniquement avec le rendu matériel.",
     "Reduz a imagem renderizada antes de a mostrar. Pode melhorar a qualidade em jogos que misturam 2D e 3D, mas "
     "deve ficar desativada em jogos só 3D. Só se aplica ao renderizador por hardware.",
     "Reduce la imagen renderizada antes de mostrarla. Puede mejorar la calidad en juegos que mezclan 2D y 3D, pero "
     "conviene desactivarla en juegos solo 3D. Solo se aplica al renderizador por hardware."))
opt("swanstation_GPU_ShaderPrecompile",
    ("Précompilation des shaders", "Pré-compilação de shaders", "Precompilación de shaders"),
    ("Définit quand les shaders de fragment du rendu matériel sont compilés. « Différée » (par défaut) les compile "
     "dans un thread d'arrière-plan pendant que le jeu démarre aussitôt ; l'ancien comportement bloquant est "
     "conservé sous « Activée ». « Désactivée » désactive la précompilation et compile chaque shader sur le thread "
     "principal à sa première utilisation : démarrage le plus rapide, mais de petites saccades possibles en début "
     "de partie. Changer le filtre de textures recompile la matrice et reprend ce coût.",
     "Define quando são compilados os shaders de fragmento do renderizador por hardware. «Diferida» (predefinição) "
     "compila-os numa thread em segundo plano enquanto o jogo arranca de imediato; o comportamento antigo, que "
     "bloqueia, mantém-se em «Ativada». «Desativada» dispensa a pré-compilação e compila cada shader na thread "
     "principal na primeira vez que é usado: arranque mais rápido, mas podem ocorrer pequenos soluços no início do "
     "jogo. Mudar o filtro de texturas recompila a matriz e volta a ter este custo.",
     "Define cuándo se compilan los shaders de fragmento del renderizador por hardware. «Diferida» "
     "(predeterminada) los compila en un hilo en segundo plano mientras el juego arranca de inmediato; el "
     "comportamiento anterior, que bloquea, se conserva en «Activada». «Desactivada» omite la precompilación y "
     "compila cada shader en el hilo principal la primera vez que se usa: arranque más rápido, pero puede haber "
     "pequeños tirones al inicio de la partida. Cambiar el filtro de texturas recompila la matriz y vuelve a pagar "
     "este costo."))
opt("swanstation_Display_ShowOSDMessages",
    ("Afficher les messages OSD", "Mostrar mensagens OSD", "Mostrar mensajes OSD"),
    ("Affiche à l'écran les messages générés par le cœur.", "Mostra no ecrã as mensagens geradas pelo núcleo.",
     "Muestra en pantalla los mensajes generados por el núcleo."))
opt("swanstation_Display_ActiveStartOffset",
    ("Décalage de début de zone active", "Deslocamento do início da área ativa",
     "Desplazamiento del inicio del área activa"),
    ("Ajoute ou retire des colonnes à gauche de l'image affichée.",
     "Adiciona ou remove colunas à esquerda da imagem mostrada.",
     "Agrega o recorta columnas a la izquierda de la imagen mostrada."))
opt("swanstation_Display_ActiveEndOffset",
    ("Décalage de fin de zone active", "Deslocamento do fim da área ativa",
     "Desplazamiento del final del área activa"),
    ("Ajoute ou retire des colonnes à droite de l'image affichée.",
     "Adiciona ou remove colunas à direita da imagem mostrada.",
     "Agrega o recorta columnas a la derecha de la imagen mostrada."))
opt("swanstation_Display_LineStartOffset",
    ("Décalage de la première ligne", "Deslocamento da primeira linha", "Desplazamiento de la primera línea"),
    ("Ajoute ou retire des lignes en haut de l'image affichée.",
     "Adiciona ou remove linhas no topo da imagem mostrada.",
     "Agrega o recorta líneas arriba de la imagen mostrada."))
opt("swanstation_Display_LineEndOffset",
    ("Décalage de la dernière ligne", "Deslocamento da última linha", "Desplazamiento de la última línea"),
    ("Ajoute ou retire des lignes en bas de l'image affichée.",
     "Adiciona ou remove linhas na base da imagem mostrada.",
     "Agrega o recorta líneas abajo de la imagen mostrada."))
opt("swanstation_MemoryCards_Card1Type",
    ("Type de carte mémoire 1 (redémarrage)", "Tipo do cartão de memória 1 (reiniciar)",
     "Tipo de tarjeta de memoria 1 (reiniciar)"),
    ("Définit le type de carte mémoire de la fente 1. Redémarrez le cœur pour changer de format. « Partagée entre "
     "tous les jeux » et les deux options « Une carte par jeu » sont conservées pour l'ancien usage et ne sont pas "
     "prises en charge. Les sauvegardes « Libretro » (.srm) et « Une carte par jeu (titre du jeu) » (.mcd) ont un "
     "format interne identique : on peut les convertir en changeant l'extension et en retirant ou ajoutant le "
     "numéro de fente (_1).",
     "Define o tipo de cartão de memória da ranhura 1. Reinicie o núcleo ao mudar de formato. «Partilhado entre "
     "todos os jogos» e as duas opções «Um cartão por jogo» servem para uso antigo e não são suportadas. As "
     "gravações «Libretro» (.srm) e «Um cartão por jogo (título do jogo)» (.mcd) têm um formato interno idêntico e "
     "podem converter-se mudando a extensão e retirando ou acrescentando o número da ranhura (_1).",
     "Define el tipo de tarjeta de memoria de la ranura 1. Reinicia el núcleo al cambiar de formato. «Compartida "
     "entre todos los juegos» y las dos opciones «Una tarjeta por juego» se conservan por compatibilidad y no "
     "tienen soporte. Las partidas «Libretro» (.srm) y «Una tarjeta por juego (título del juego)» (.mcd) tienen un "
     "formato interno idéntico y se pueden convertir cambiando la extensión y quitando o agregando el número de "
     "ranura (_1)."))
opt("swanstation_MemoryCards_Card2Type",
    ("Type de carte mémoire 2", "Tipo do cartão de memória 2", "Tipo de tarjeta de memoria 2"),
    ("Définit le type de carte mémoire de la fente 2.", "Define o tipo de cartão de memória da ranhura 2.",
     "Define el tipo de tarjeta de memoria de la ranura 2."))
opt("swanstation_MemoryCards_UsePlaylistTitle",
    ("Une seule carte pour la liste de lecture", "Um só cartão para a lista", "Una sola tarjeta para la lista"),
    ("Avec une liste de lecture (m3u) et des cartes par jeu (titre), une seule carte est utilisée pour tous les "
     "disques. Sinon, chaque disque a sa propre carte.",
     "Com uma lista (m3u) e cartões por jogo (título), usa-se um único cartão para todos os discos. Se estiver "
     "desativado, cada disco usa o seu cartão.",
     "Con una lista (m3u) y tarjetas por juego (título), se usa una sola tarjeta para todos los discos. Si está "
     "desactivado, cada disco usa su propia tarjeta."))
opt("swanstation_ControllerPorts_MultitapMode",
    ("Mode multitap", "Modo multitap", "Modo multitap"),
    ("Définit le mode du multitap.", "Define o modo do multitap.", "Define el modo del multitap."))
opt("swanstation_Controller_AnalogCombo",
    ("Combinaison du mode analogique", "Combinação do modo analógico", "Combinación del modo analógico"),
    ("Définit la combinaison de touches qui active ou désactive le mode analogique.",
     "Define a combinação de botões que ativa ou desativa o modo analógico.",
     "Define la combinación de botones que activa o desactiva el modo analógico."))
opt("swanstation_Controller_EnableRumble",
    ("Activer les vibrations", "Ativar vibração", "Activar vibración"),
    ("Active le retour haptique avec une manette à vibration et un périphérique d'entrée compatible.",
     "Ativa o retorno háptico com um comando com vibração e um dispositivo de entrada compatível.",
     "Activa el retorno háptico con un control con vibración y un dispositivo de entrada compatible."))
opt("swanstation_Controller_ShowCrosshair",
    ("Afficher le viseur du GunCon", "Mostrar a mira do GunCon", "Mostrar la mira del GunCon"),
    ("Affiche un viseur quand le périphérique d'entrée est un « Namco GunCon ».",
     "Mostra uma mira quando o dispositivo de entrada é um «Namco GunCon».",
     "Muestra una mira cuando el dispositivo de entrada es un «Namco GunCon»."))

# Controller N: (name in fr, pt, es with {n}), help
CONTROLLER = {
    "ForceAnalog": (
        ("Manette {n} : forcer le mode analogique", "Comando {n}: forçar modo analógico",
         "Control {n}: forzar modo analógico"),
        ("Force le mode analogique en permanence. Peut poser problème dans certains jeux. À n'utiliser que pour les "
         "jeux compatibles avec le mode analogique qui ne l'activent pas eux-mêmes. Désactivé, le mode analogique se "
         "bascule en maintenant la combinaison du mode analogique du DualShock.",
         "Força o modo analógico a estar sempre ativo. Pode causar problemas em alguns jogos. Use apenas em jogos que "
         "suportam o modo analógico mas não o ativam sozinhos. Desativado, o modo analógico alterna-se mantendo "
         "premida a combinação do modo analógico do DualShock.",
         "Fuerza el modo analógico siempre activo. Puede causar problemas en algunos juegos. Úsalo solo en juegos que "
         "admiten el modo analógico pero no lo activan por sí mismos. Desactivado, el modo analógico se alterna "
         "manteniendo presionada la combinación del modo analógico del DualShock.")),
    "AnalogDPadInDigitalMode": (
        ("Manette {n} : sticks analogiques comme croix en mode numérique",
         "Comando {n}: sticks analógicos como cruzeta em modo digital",
         "Control {n}: sticks analógicos como cruceta en modo digital"),
        ("Permet d'utiliser les sticks analogiques pour la croix directionnelle en mode numérique, en plus des "
         "boutons.",
         "Permite usar os sticks analógicos para controlar a cruzeta em modo digital, além dos botões.",
         "Permite usar los sticks analógicos para controlar la cruceta en modo digital, además de los botones.")),
    "AxisScale": (
        ("Manette {n} : échelle des axes analogiques", "Comando {n}: escala dos eixos analógicos",
         "Control {n}: escala de los ejes analógicos"),
        ("Définit le facteur d'échelle des axes des sticks analogiques.",
         "Define o fator de escala dos eixos dos sticks analógicos.",
         "Define el factor de escala de los ejes de los sticks analógicos.")),
    "VibrationBias": (
        ("Manette {n} : biais de vibration", "Comando {n}: desvio da vibração", "Control {n}: sesgo de vibración"),
        ("Applique un décalage aux intensités de vibration : plus la valeur est élevée, plus les petites vibrations "
         "se remarquent.",
         "Aplica um desvio às intensidades da vibração; valores mais altos tornam as vibrações pequenas mais "
         "notórias.",
         "Aplica un desvío a las intensidades de vibración; valores más altos hacen más notorias las vibraciones "
         "pequeñas.")),
    "XScale": (
        ("Manette {n} : échelle X du pistolet", "Comando {n}: escala X da pistola", "Control {n}: escala X de la pistola"),
        ("Met à l'échelle les coordonnées X par rapport au centre de l'écran.",
         "Escala as coordenadas X em relação ao centro do ecrã.",
         "Escala las coordenadas X respecto al centro de la pantalla.")),
    "YScale": (
        ("Manette {n} : échelle Y du pistolet", "Comando {n}: escala Y da pistola", "Control {n}: escala Y de la pistola"),
        ("Met à l'échelle les coordonnées Y par rapport au centre de l'écran.",
         "Escala as coordenadas Y em relação ao centro do ecrã.",
         "Escala las coordenadas Y respecto al centro de la pantalla.")),
    "SteeringDeadzone": (
        ("Manette {n} : zone morte de direction (NeGcon)", "Comando {n}: zona morta da direção (NeGcon)",
         "Control {n}: zona muerta de dirección (NeGcon)"),
        ("Définit la taille de la zone morte de l'axe de direction.",
         "Define o tamanho da zona morta do eixo de direção.",
         "Define el tamaño de la zona muerta del eje de dirección.")),
    "TwistResponse": (
        ("Manette {n} : réponse de torsion (NeGcon)", "Comando {n}: resposta da torção (NeGcon)",
         "Control {n}: respuesta de torsión (NeGcon)"),
        ("Définit le type de réponse de la torsion pour le stick gauche. « Quadratique » est plus précis que "
         "« Linéaire » pour les petits mouvements et convient mieux aux manettes modernes ; « Cubique » augmente "
         "encore cette précision mais « exagère » les grands mouvements. « Linéaire » ne sert généralement qu'avec "
         "des volants.",
         "Define o tipo de resposta da torção do stick esquerdo. «Quadrática» permite mais precisão do que «Linear» "
         "em movimentos pequenos e convém mais aos comandos modernos; «Cúbica» aumenta ainda mais essa precisão, mas "
         "«exagera» os movimentos grandes. «Linear» só costuma ser útil com volantes.",
         "Define el tipo de respuesta de la torsión del stick izquierdo. «Cuadrática» permite más precisión que "
         "«Lineal» en movimientos pequeños y es más adecuada para controles modernos; «Cúbica» aumenta aún más esa "
         "precisión pero «exagera» los movimientos grandes. «Lineal» solo suele servir con volantes.")),
}
for n in range(1, 9):
    for suffix, (names, helps) in CONTROLLER.items():
        opt(f"swanstation_Controller{n}_{suffix}", tuple(x.format(n=n) for x in names), helps)

opt("swanstation_CDROM_ReadaheadSectors",
    ("Lecture anticipée asynchrone du CD-ROM", "Leitura antecipada assíncrona do CD-ROM",
     "Lectura anticipada asíncrona del CD-ROM"),
    ("Détermine jusqu'où le thread du CD-ROM lit à l'avance. Peut réduire les saccades sur un stockage lent ou avec "
     "des jeux compressés.",
     "Determina até onde o thread do CD-ROM lê antecipadamente. Pode reduzir soluços em armazenamento lento ou com "
     "jogos comprimidos.",
     "Determina hasta dónde lee por adelantado el hilo del CD-ROM. Puede reducir los tirones en almacenamiento "
     "lento o con juegos comprimidos."))
opt("swanstation_CPU_Overclock",
    ("Overclocking du CPU", "Overclock do CPU", "Overclock de la CPU"),
    ("Fait tourner le CPU émulé plus vite ou plus lentement que la vitesse native, ce qui peut améliorer la "
     "fluidité de certains jeux. Casse d'autres jeux et augmente la configuration requise : à utiliser avec "
     "prudence.",
     "Faz o CPU emulado funcionar mais depressa ou mais devagar do que a velocidade nativa, o que pode melhorar a "
     "fluidez de alguns jogos. Estraga outros jogos e aumenta os requisitos: use com cuidado.",
     "Hace funcionar la CPU emulada más rápido o más lento que la velocidad nativa, lo que puede mejorar los cuadros "
     "por segundo de algunos juegos. Rompe otros juegos y aumenta los requisitos: úsalo con cuidado."))
opt("swanstation_Main_ApplyGameSettings",
    ("Appliquer les réglages de compatibilité", "Aplicar definições de compatibilidade",
     "Aplicar ajustes de compatibilidad"),
    ("Désactive automatiquement les améliorations pour les jeux qui y sont incompatibles.",
     "Desativa automaticamente as melhorias nos jogos que são incompatíveis com elas.",
     "Desactiva automáticamente las mejoras en los juegos que no son compatibles."))
opt("swanstation_Logging_LogLevel",
    ("Niveau de journalisation", "Nível de registo", "Nivel de registro"),
    ("Définit le niveau d'information consigné par le cœur.", "Define o nível de informação registado pelo núcleo.",
     "Define el nivel de información que registra el núcleo."))
opt("swanstation_CPU_RecompilerICache",
    ("ICache du recompilateur CPU", "ICache do recompilador do CPU", "ICache del recompilador de CPU"),
    ("Détermine si le cache d'instructions du CPU est simulé dans le recompilateur. Améliore la précision pour un "
     "petit coût en performances. Si des jeux vont trop vite, essayez de l'activer.",
     "Determina se a cache de instruções do CPU é simulada no recompilador. Melhora a precisão com um pequeno custo "
     "de desempenho. Se os jogos andarem demasiado depressa, tente ativá-la.",
     "Determina si la caché de instrucciones de la CPU se simula en el recompilador. Mejora la precisión con un "
     "pequeño costo de rendimiento. Si los juegos van demasiado rápido, prueba a activarla."))
opt("swanstation_CPU_RecompilerBlockLinking",
    ("Chaînage de blocs du recompilateur CPU", "Ligação de blocos do recompilador do CPU",
     "Enlace de bloques del recompilador de CPU"),
    ("Permet au code généré de sauter directement d'un bloc à l'autre sans passer par le répartiteur. Apporte un "
     "gain de vitesse mesurable.",
     "Permite que o código gerado salte diretamente entre blocos sem passar pelo despachante. Dá um ganho de "
     "velocidade mensurável.",
     "Permite que el código generado salte directamente entre bloques sin pasar por el despachador. Da una mejora "
     "de velocidad medible."))
opt("swanstation_CPU_FastmemRewrite",
    ("Réécriture de l'accès mémoire rapide du recompilateur CPU",
     "Reescrita do acesso rápido à memória do recompilador do CPU",
     "Reescritura del acceso rápido a memoria del recompilador de CPU"),
    ("Active les réécritures quand l'accès mémoire rapide du recompilateur est activé. Peut accélérer de façon "
     "mesurable, mais peut faire planter le cœur sur certaines plateformes.",
     "Ativa as reescritas quando o acesso rápido à memória do recompilador está ativado. Pode acelerar de forma "
     "mensurável, mas pode fazer o núcleo falhar em certas plataformas.",
     "Activa las reescrituras cuando el acceso rápido a memoria del recompilador está activado. Puede acelerar de "
     "forma medible, pero puede hacer que el núcleo falle en ciertas plataformas."))
opt("swanstation_TextureReplacements_EnableVRAMWriteReplacements",
    ("Activer le remplacement des textures d'écriture VRAM",
     "Ativar substituição de texturas de escrita na VRAM",
     "Activar reemplazo de texturas de escritura en VRAM"),
    ("Remplace les « textures d'écriture VRAM » par des packs de textures au format DuckStation du dossier "
     "« swanstation/textures » du répertoire système. Uniquement avec les rendus Vulkan et D3D11 (si disponible).",
     "Substitui as «texturas de escrita na VRAM» por packs de texturas no formato DuckStation da pasta "
     "«swanstation/textures» do diretório do sistema. Só funciona com os renderizadores Vulkan e D3D11 (onde "
     "existir).",
     "Reemplaza las «texturas de escritura en VRAM» por packs de texturas en formato DuckStation de la carpeta "
     "«swanstation/textures» del directorio del sistema. Solo funciona con los renderizadores Vulkan y D3D11 (donde "
     "exista)."))
opt("swanstation_TextureReplacements_PreloadTextures",
    ("Précharger les textures de remplacement", "Pré-carregar texturas de substituição",
     "Precargar texturas de reemplazo"),
    ("Précharge en RAM les textures de remplacement d'écriture VRAM.",
     "Pré-carrega na RAM as texturas de substituição de escrita na VRAM.",
     "Precarga en RAM las texturas de reemplazo de escritura en VRAM."))
opt("swanstation_Console_Enable8MBRAM",
    ("Activer 8 Mo de RAM (console de développement)", "Ativar 8 MB de RAM (consola de desenvolvimento)",
     "Activar 8 MB de RAM (consola de desarrollo)"),
    ("Active 6 Mo de RAM supplémentaires, présents sur les consoles de développement. Les jeux doivent utiliser un "
     "tas plus grand pour en profiter, et ceux qui s'appuient sur le miroir de mémoire peuvent cesser de "
     "fonctionner : à réserver aux mods compatibles.",
     "Ativa 6 MB adicionais de RAM, presentes nas consolas de desenvolvimento. Os jogos têm de usar um heap maior "
     "para os aproveitar e os que dependem do espelhamento de memória podem deixar de funcionar: use só com mods "
     "compatíveis.",
     "Activa 6 MB adicionales de RAM, presentes en las consolas de desarrollo. Los juegos deben usar un heap mayor "
     "para aprovecharlos y los que dependen del espejado de memoria pueden dejar de funcionar: úsalo solo con mods "
     "compatibles."))
opt("swanstation_Hacks_OldMDECRoutines",
    ("Utiliser les anciennes routines MDEC", "Usar rotinas MDEC antigas", "Usar rutinas MDEC antiguas"),
    ("Utilise les anciennes routines pour le contenu MDEC. Les nouvelles peuvent être plus belles dans certains "
     "jeux, moins dans d'autres. Le remplacement de textures d'écriture VRAM exige l'une ou l'autre : choisissez "
     "celle qu'il faut.",
     "Usa as rotinas antigas para o conteúdo MDEC. As novas podem ficar melhor em alguns jogos e pior noutros. A "
     "substituição de texturas de escrita na VRAM exige umas ou outras: escolha as necessárias.",
     "Usa las rutinas antiguas para el contenido MDEC. Las nuevas pueden verse mejor en algunos juegos y peor en "
     "otros. El reemplazo de texturas de escritura en VRAM necesita unas u otras: elige las que haga falta."))
opt("swanstation_Audio_FastHook",
    ("Lots audio par image (redémarrage)", "Agrupar áudio por imagem (reiniciar)",
     "Agrupar audio por cuadro (reiniciar)"),
    ("Transmet tous les échantillons SPU au frontend une fois par image émulée, au lieu de les envoyer au fil de "
     "leur production. C'est plus rapide, avec un audio plus régulier, et c'est le bon choix pour presque tous les "
     "jeux. Quelques titres (notamment Formula 1 / Formula 1 '97) plantent au démarrage si l'audio n'est pas livré "
     "en cours d'image ; le cœur désactive l'option pour ces titres connus, mais si un jeu se fige au démarrage "
     "avec l'audio activé, essayez de la désactiver.",
     "Entrega todas as amostras do SPU ao frontend uma vez por imagem emulada, em vez de as enviar à medida que são "
     "produzidas. É mais rápido, dá um áudio mais uniforme e é a escolha certa para quase todos os jogos. Alguns "
     "títulos (nomeadamente Formula 1 / Formula 1 '97) bloqueiam no arranque se o áudio não for entregue a meio da "
     "imagem; o núcleo desativa esta opção nesses títulos conhecidos, mas se um jogo congelar no arranque com o "
     "áudio ativado, tente desativá-la.",
     "Entrega todas las muestras del SPU al frontend una vez por cuadro emulado, en vez de enviarlas a medida que "
     "se producen. Es más rápido, da un audio más uniforme y es la opción correcta para casi todos los juegos. "
     "Unos pocos títulos (notablemente Formula 1 / Formula 1 '97) se cuelgan al arrancar si el audio no se entrega "
     "a mitad del cuadro; el núcleo desactiva esta opción en esos títulos conocidos, pero si un juego se congela al "
     "iniciar con el audio activado, prueba a desactivarla."))

# ---------------------------------------------------------------- value names: English -> (fr, pt, es)
VALUES = {
    "Adaptive (Preserve 3D/Smooth 2D)": ("Adaptatif (préserve la 3D / lisse la 2D)",
                                         "Adaptativo (preserva 3D / suaviza 2D)",
                                         "Adaptativo (preserva 3D / suaviza 2D)"),
    "All Borders": ("Toutes les bordures", "Todas as margens", "Todos los bordes"),
    "Bilinear": ("Bilinéaire", "Bilinear", "Bilineal"),
    "Bilinear (No Edge Blending)": ("Bilinéaire (sans fondu des bords)", "Bilinear (sem mistura de arestas)",
                                    "Bilineal (sin mezcla de bordes)"),
    "Box (Downsample 3D/Smooth All)": ("Box (réduit la 3D / lisse tout)", "Box (reduz 3D / suaviza tudo)",
                                       "Box (reduce 3D / suaviza todo)"),
    "Cached Interpreter": ("Interpréteur avec cache", "Intérprete com cache", "Intérprete con caché"),
    "Corrected (NTSC)": ("Corrigé (NTSC)", "Corrigida (NTSC)", "Corregida (NTSC)"),
    "Corrected (Region Native)": ("Corrigé (natif de la région)", "Corrigida (nativa da região)",
                                  "Corregida (nativa de la región)"),
    "Cubic": ("Cubique", "Cúbica", "Cúbica"),
    "Custom": ("Personnalisé", "Personalizado", "Personalizado"),
    "Debug": ("Débogage", "Depuração", "Depuración"),
    "Developer": ("Développeur", "Programador", "Desarrollador"),
    "Disabled": ("Désactivé", "Desativado", "Desactivado"),
    "Disabled (Synchronous)": ("Désactivé (synchrone)", "Desativado (síncrono)", "Desactivado (síncrono)"),
    "Disabled (compile on first use)": ("Désactivée (compilation à la première utilisation)",
                                        "Desativada (compila na primeira utilização)",
                                        "Desactivada (compila al primer uso)"),
    "Enable on Port 1 Only": ("Activé sur le port 1 uniquement", "Ativado só na porta 1",
                              "Activado solo en el puerto 1"),
    "Enable on Port 2 Only": ("Activé sur le port 2 uniquement", "Ativado só na porta 2",
                              "Activado solo en el puerto 2"),
    "Enable on Ports 1 and 2": ("Activé sur les ports 1 et 2", "Ativado nas portas 1 e 2",
                                "Activado en los puertos 1 y 2"),
    "Enabled": ("Activé", "Ativado", "Activado"),
    "Enabled (block until done, like old behaviour)": ("Activée (bloque jusqu'à la fin, comme avant)",
                                                       "Ativada (bloqueia até terminar, como antes)",
                                                       "Activada (bloquea hasta terminar, como antes)"),
    "Error": ("Erreur", "Erro", "Error"),
    "Infinite/Instantaneous": ("Infini / instantané", "Infinito / instantâneo", "Infinito / instantáneo"),
    "Information": ("Informations", "Informação", "Información"),
    "Interpreter": ("Interpréteur", "Intérprete", "Intérprete"),
    "JINC2 (No Edge Blending)": ("JINC2 (sans fondu des bords)", "JINC2 (sem mistura de arestas)",
                                 "JINC2 (sin mezcla de bordes)"),
    "Lazy (background thread, default)": ("Différée (thread d'arrière-plan, par défaut)",
                                          "Diferida (thread em segundo plano, predefinição)",
                                          "Diferida (hilo en segundo plano, predeterminada)"),
    "Libretro (Default)": ("Libretro (par défaut)", "Libretro (predefinição)", "Libretro (predeterminado)"),
    "Linear": ("Linéaire", "Linear", "Lineal"),
    "Nearest-Neighbor": ("Plus proche voisin", "Vizinho mais próximo", "Vecino más cercano"),
    "No Memory Card": ("Aucune carte mémoire", "Sem cartão de memória", "Sin tarjeta de memoria"),
    "None": ("Aucun", "Nenhum", "Ninguno"),
    "None (Double Speed)": ("Aucun (vitesse double)", "Nenhum (velocidade dupla)", "Ninguno (velocidad doble)"),
    "Only Overscan Area": ("Zone de surbalayage uniquement", "Apenas a área de overscan", "Solo el área de overscan"),
    "Performance": ("Performances", "Desempenho", "Rendimiento"),
    "Profile": ("Profil", "Perfil", "Perfil"),
    "Quadratic": ("Quadratique", "Quadrática", "Cuadrática"),
    "Recompiler": ("Recompilateur", "Recompilador", "Recompilador"),
    "Separate Card Per Game (Game Code)": ("Une carte par jeu (code du jeu)", "Um cartão por jogo (código do jogo)",
                                           "Una tarjeta por juego (código del juego)"),
    "Separate Card Per Game (Game Title)": ("Une carte par jeu (titre du jeu)", "Um cartão por jogo (título do jogo)",
                                            "Una tarjeta por juego (título del juego)"),
    "Shared Between All Games": ("Partagée entre tous les jeux", "Partilhado entre todos os jogos",
                                 "Compartida entre todos los juegos"),
    "Success": ("Succès", "Sucesso", "Éxito"),
    "Trace": ("Trace", "Rasto", "Traza"),
    "Uncorrected (PAR 1:1)": ("Non corrigé (PAR 1:1)", "Sem correção (PAR 1:1)", "Sin corregir (PAR 1:1)"),
    "Warning": ("Avertissement", "Aviso", "Advertencia"),
    "xBR (No Edge Blending)": ("xBR (sans fondu des bords)", "xBR (sem mistura de arestas)",
                               "xBR (sin mezcla de bordes)"),
    "100% (Default)": ("100 % (par défaut)", "100% (predefinição)", "100% (predeterminado)"),
}

GROUPS = {
    "Console (Core)": ("Console (cœur)", "Consola (Núcleo)", "Consola (Núcleo)"),
    "Enhancements (Core)": ("Améliorations (cœur)", "Melhorias (Núcleo)", "Mejoras (Núcleo)"),
    "Display (Core)": ("Affichage (cœur)", "Ecrã (Núcleo)", "Pantalla (Núcleo)"),
    "Controller ports (Core)": ("Ports manettes (cœur)", "Portas de comandos (Núcleo)", "Puertos de control (Núcleo)"),
    "Advanced (Core)": ("Avancé (cœur)", "Avançado (Núcleo)", "Avanzado (Núcleo)"),
}


def pattern_value(text):
    """Values that follow a pattern: 'N Sectors (7KB / 2ms)', '2x (Quad Speed)', '1.5 pixels'."""
    m = re.fullmatch(r"(\d+) Sectors? \((\d+)KB / (\d+)ms\)", text)
    if m:
        n, kb, ms = m.groups()
        one = n == "1"
        return (f"{n} secteur{'' if one else 's'} ({kb} Ko / {ms} ms)",
                f"{n} {'setor' if one else 'setores'} ({kb} KB / {ms} ms)",
                f"{n} {'sector' if one else 'sectores'} ({kb} KB / {ms} ms)")
    m = re.fullmatch(r"(\d+)x \((\w+|\d+x) Speed\)", text)
    if m:
        n, spd = m.groups()
        spd = {"Quad": "4x", "Double": "2x"}.get(spd, spd)
        return (f"{n}x (vitesse {spd})", f"{n}x (velocidade {spd})", f"{n}x (velocidad {spd})")
    m = re.fullmatch(r"([\d.]+) pixels", text)
    if m:
        return (f"{m.group(1)} pixels", f"{m.group(1)} pixéis", f"{m.group(1)} píxeles")
    return None


def main():
    src = (ROOT / "src/core/swanstation_options.c").read_text()
    opts = re.findall(r'\{"(swanstation_\w+)", "((?:[^"\\]|\\.)*)",\n     "((?:[^"\\]|\\.)*)",\n     SSC_\w+', src)
    existing = set(c_unescape(m) for m in re.findall(r'\{\{"((?:[^"\\]|\\.)*)"',
                                                    (ROOT / "src/i18n.c").read_text()))
    entries, seen = [], set()

    def add(en, tr3):
        if en in seen or en in existing:
            return
        seen.add(en)
        fr, pt, es = tr3
        entries.append((en, fr, pt, es))

    missing = []
    for key, label, help_ in opts:
        label, help_ = c_unescape(label), c_unescape(help_)
        if key not in LABEL:
            missing.append(key)
            continue
        add(label, LABEL[key])
        if help_:
            add(help_, HELP[key])
    if missing:
        raise SystemExit("no translation for: " + ", ".join(missing))

    values = set()
    for m in re.finditer(r"static const char \*const l\d+\[\] = \{(.*?)\};", src):
        values |= {c_unescape(x) for x in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))}
    for v in sorted(values):
        t = VALUES.get(v) or pattern_value(v)
        if t:
            add(v, t)
    for en, t in GROUPS.items():
        add(en, t)

    out = ["/* Generated by tools/gen_swanstation_i18n.py - do not edit. SwanStation's core options in French,",
           " * Portuguese and Spanish; Japanese keeps the English text. Included at the end of ENTRIES (i18n.c). */"]
    for en, fr, pt, es in entries:
        out.append("    {{" + ", ".join(c_str(x) for x in (en, fr, pt, es, en)) + "}},")
    (ROOT / "src/i18n_swanstation.inc").write_text("\n".join(out) + "\n")
    print(len(entries), "entries")


main()
