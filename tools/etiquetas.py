#!/usr/bin/env python3
"""Pone en las paginas lo que necesita un buscador para entenderlas y para
enseñar una tarjeta al compartir el enlace.

Tres cosas y ninguna es el texto de la pagina, que ya estaba:

  canonical   GitHub Pages sirve /Hixean/ y /Hixean/index.html como dos
              direcciones del mismo documento. Sin canonical son dos paginas
              distintas con el mismo contenido y el buscador tiene que elegir.
  og:         lo que se ve al compartir. Sin esto, un enlace a Hixean es una
              URL pelada en Slack, Discord o Telegram.
  JSON-LD     lo que le da al buscador la ficha del proyecto: lenguaje, licencia,
              repositorio y version. Es de lo poco que decide como un resultado
              se muestra en lugar de ser un enlace azul.

El base https://wwwillcoxon.github.io/Hixean sale del dominio de Pages, asi que
esta escrito aqui en vez de deducido: si se deduce mal, el canonical manda a
otro sitio y es peor que no tenerlo.
"""
import re
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
BASE = "https://wwwillcoxon.github.io/Hixean"

COMUN = f"""<link rel="canonical" href="{BASE}/PAGINA">
<link rel="icon" href="favicon.svg" type="image/svg+xml">
<link rel="alternate icon" href="favicon-32.png" sizes="32x32" type="image/png">
<link rel="alternate icon" href="favicon-16.png" sizes="16x16" type="image/png">
<link rel="apple-touch-icon" href="apple-touch-icon.png">
<link rel="manifest" href="site.webmanifest">
<meta name="theme-color" content="#0f0e0c">
<meta property="og:site_name" content="Hixean">
<meta property="og:locale" content="es_ES">
<meta property="og:type" content="website">
<meta property="og:url" content="{BASE}/PAGINA">
<meta property="og:image" content="{BASE}/og-hixean.png">
<meta property="og:image:width" content="1200">
<meta property="og:image:height" content="630">
<meta property="og:image:alt" content="Hixean: el nombre sobre un panel oscuro y tres barras de codigo, y la linea 8 896 bytes.">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:title" content="TITULO">
<meta name="twitter:description" content="DESCRIPCION">
<meta name="twitter:image" content="{BASE}/og-hixean.png">"""

# El JSON-LD va dentro de la pagina, no en el head: es datos, y va con datos.
JSONLD = """<script type="application/ld+json">
{
  "@context": "https://schema.org",
  "@type": "SoftwareSourceCode",
  "name": "Hixean",
  "url": "https://wwwillcoxon.github.io/Hixean/",
  "codeRepository": "https://github.com/wwwillcoxon/Hixean",
  "license": "https://github.com/wwwillcoxon/Hixean/blob/main/LICENSE",
  "programmingLanguage": "Hixean",
  "runtimePlatform": "Linux x86-64",
  "applicationCategory": "DeveloperApplication",
  "description": "Lenguaje de tipos estaticos que compila a C11 sin maquina virtual, sin unwinding y sin libc por defecto: hola mundo ocupa 8 896 bytes.",
  "keywords": "lenguaje de programacion, compilador, C11, AOT, sin recolector, QBasic, estatico",
  "author": { "@type": "Person", "name": "wwwillcoxon", "url": "https://github.com/wwwillcoxon" },
  "offers": { "@type": "Offer", "price": "0", "priceCurrency": "USD" }
}
</script>"""


def escapar(s):
    return s.replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;")


def meta_de(pagina, titulo, descripcion):
    comun = COMUN.replace("PAGINA", pagina)
    comun = comun.replace("TITULO", escapar(titulo))
    comun = comun.replace("DESCRIPCION", escapar(descripcion))
    og_titulo = (f'<meta property="og:title" content="{escapar(titulo)}">\n'
                 f'<meta property="og:description" content="{escapar(descripcion)}">\n')
    return og_titulo + comun


def insertar(ruta, bloque, tras="<link rel=\"stylesheet\" href=\"style.css\">"):
    """Mete el bloque justo despues de la hoja de estilos, que es donde el
    navegador espera encontrarlo y donde ya no hay nada."""
    s = ruta.read_text(encoding="utf-8")
    if "canonical" in s:
        print(f"nada     {ruta.name}: ya tiene canonical")
        return 0
    if tras not in s:
        print(f"MAL      {ruta.name}: no encuentro donde insertar")
        return 1
    s = s.replace(tras, tras + "\n" + bloque, 1)
    ruta.write_text(s, encoding="utf-8")
    print(f"ok       {ruta.name}: canonical, Open Graph, twitter:card y favicon")
    return 0


def principal():
    fallos = 0
    fallos += insertar(
        RAIZ / "site/index.html",
        meta_de("", "Hixean — lenguaje AOT que compila a C11",
                "Lenguaje compilado, estático y estándar, evolución moderna de QBasic. "
                "Sin máquina virtual, sin unwinding y sin libc por defecto: hola mundo "
                "ocupa 8 896 bytes.")
        .replace('href="https://wwwillcoxon.github.io/Hixean/"', 'href="https://wwwillcoxon.github.io/Hixean/"'),
    )
    # JSON-LD al final del body, antes de que empiece el pie
    s = (RAIZ / "site/index.html").read_text(encoding="utf-8")
    if "application/ld+json" not in s:
        s = s.replace("<footer", JSONLD + "\n\n<footer", 1)
        (RAIZ / "site/index.html").write_text(s, encoding="utf-8")
        print("ok       index.html: JSON-LD SoftwareSourceCode")

    fallos += insertar(
        RAIZ / "site/directorio.html",
        meta_de("directorio.html", "Directorio — Hixean",
                "Todo lo que hay para leer sobre Hixean: guía de programación en PDF, "
                "manual interactivo, gramática, ADR, changelog, código y la lista corta "
                "de lo que el lenguaje no tiene."))
    fallos += insertar(
        RAIZ / "site/terminos.html",
        meta_de("terminos.html", "Términos — Hixean",
                "Términos de uso de Hixean: licencia MIT, aviso legal, privacidad y uso "
                "aceptable. Sin analítica, sin cookies y sin recogida de datos."))
    return fallos


def manual():
    """El manual esta en docs/ y sale publicado en /docs/manual.html, asi que su
    ruta no es la del directorio del sitio: los enlaces relatives de la pagina
    dicen ../."""
    ruta = RAIZ / "docs/manual.html"
    s = ruta.read_text(encoding="utf-8")
    if "canonical" in s:
        print("nada     manual.html: ya tiene canonical")
        return 0
    titulo = "Hixean: manual de programación"
    desc = ("Manual interactivo de Hixean: los 88 códigos de diagnóstico con su fila, "
            "los tipos, los operadores y las reglas, con ejemplos que compilan.")
    bloque = (f'<meta name="description" content="{desc}">\n'
              f'<link rel="canonical" href="{BASE}/docs/manual.html">\n'
              f'<link rel="icon" href="../site/favicon.svg" type="image/svg+xml">\n'
              f'<link rel="apple-touch-icon" href="../site/apple-touch-icon.png">\n'
              f'<meta property="og:type" content="article">\n'
              f'<meta property="og:title" content="{titulo}">\n'
              f'<meta property="og:description" content="{desc}">\n'
              f'<meta property="og:url" content="{BASE}/docs/manual.html">\n'
              f'<meta property="og:image" content="{BASE}/og-hixean.png">\n'
              f'<meta name="twitter:card" content="summary_large_image">\n'
              f'<meta name="twitter:title" content="{titulo}">\n'
              f'<meta name="twitter:description" content="{desc}">\n'
              f'<meta name="twitter:image" content="{BASE}/og-hixean.png">')
    # El manual no usa style.css: lleva su propio <style> en linea porque el tema
    # claro y el oscuro son reglas distintas. Se ancla al title, que es lo unico
    # que esta en todos los<head>.
    ancla = f"<title>{titulo}</title>"
    if ancla not in s:
        print("MAL      manual.html: no encuentro el title")
        return 1
    s = s.replace(ancla, ancla + "\n" + bloque, 1)
    ruta.write_text(s, encoding="utf-8")
    print("ok       manual.html: description, canonical, Open Graph y twitter:card")
    return 0


def manifiesto():
    """El manifest del sitio. Chrome lo pide cuando alguien instala la pagina, y
    sin esto avisa por consola. Es JSON de verdad, no un PNG renombrado: un
    manifest que no es JSON se rechaza entero."""
    m = """{
  "name": "Hixean",
  "short_name": "Hixean",
  "lang": "es",
  "start_url": "../",
  "scope": "../",
  "display": "standalone",
  "background_color": "#0f0e0c",
  "theme_color": "#0f0e0c",
  "icons": [
    { "src": "favicon-32.png", "sizes": "32x32", "type": "image/png" },
    { "src": "apple-touch-icon.png", "sizes": "180x180", "type": "image/png" },
    { "src": "icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "any" }
  ]
}
"""
    (RAIZ / "site/site.webmanifest").write_text(m, encoding="utf-8")
    print("ok       site/site.webmanifest")
    return 0


def sitemap():
    """El sitemap lo escribe el workflow de Pages desde esta lista, para que no
    se quede viejo el dia que se añada una pagina."""
    paginas = ["", "directorio.html", "terminos.html", "docs/manual.html"]
    dias = "2026-10-04"
    xml = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">']
    for p in paginas:
        xml.append("  <url>")
        xml.append(f"    <loc>{BASE}/{p}</loc>" if p else f"    <loc>{BASE}/</loc>")
        xml.append(f"    <lastmod>{dias}</lastmod>")
        xml.append("  </url>")
    xml.append("</urlset>")
    (RAIZ / "site/sitemap.xml").write_text("\n".join(xml) + "\n", encoding="utf-8")
    print(f"ok       site/sitemap.xml ({len(paginas)} paginas)")

    robots = ["# Todo el sitio es publico y no hay nada que esconder.",
              "User-agent: *",
              "Allow: /",
              "",
              "# El manual vive fuera del directorio del sitio y tambien se indexa.",
              f"Sitemap: {BASE}/sitemap.xml",
              ""]
    (RAIZ / "site/robots.txt").write_text("\n".join(robots), encoding="utf-8")
    print("ok       site/robots.txt")
    return 0


if __name__ == "__main__":
    sys.exit(principal() + manual() + manifiesto() + sitemap())