// Lo único que hace esta página: tema, pestañas, copiar, contadores,
// scroll-spy y aparecer al bajar. Sin dependencias y sin red.

(function () {
  "use strict";

  var raiz = document.documentElement;
  var reducir = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  /* ---------- tema ---------- */

  var botonTema = document.getElementById("tema");

  function aplicarTema(oscuro) {
    raiz.setAttribute("data-tema", oscuro ? "oscuro" : "claro");
    botonTema.setAttribute("aria-pressed", oscuro ? "true" : "false");
    botonTema.querySelector(".texto").textContent = oscuro ? "Claro" : "Oscuro";
    try {
      localStorage.setItem("hixean-tema", oscuro ? "oscuro" : "claro");
    } catch (e) {
      /* sin localStorage el tema dura lo que dura la pestaña */
    }
  }

  var guardado = null;
  try {
    guardado = localStorage.getItem("hixean-tema");
  } catch (e) {
    guardado = null;
  }
  if (!guardado && window.matchMedia("(prefers-color-scheme: light)").matches) {
    guardado = "claro";
  }
  aplicarTema(guardado !== "claro");

  botonTema.addEventListener("click", function () {
    aplicarTema(raiz.getAttribute("data-tema") !== "oscuro");
  });

  /* ---------- pestañas ---------- */

  Array.prototype.forEach.call(document.querySelectorAll("[data-pestanas]"), function (grupo) {
    var botones = grupo.querySelectorAll('[role="tab"]');
    var paneles = grupo.querySelectorAll('[role="tabpanel"]');

    function mostrar(indice) {
      Array.prototype.forEach.call(botones, function (b, i) {
        b.setAttribute("aria-selected", i === indice ? "true" : "false");
        b.tabIndex = i === indice ? 0 : -1;
      });
      Array.prototype.forEach.call(paneles, function (p, i) {
        p.hidden = i !== indice;
      });
    }

    Array.prototype.forEach.call(botones, function (b, i) {
      b.addEventListener("click", function () {
        mostrar(i);
      });
      b.addEventListener("keydown", function (e) {
        var salto = e.key === "ArrowRight" ? 1 : e.key === "ArrowLeft" ? -1 : 0;
        if (!salto) return;
        e.preventDefault();
        var siguiente = (i + salto + botones.length) % botones.length;
        mostrar(siguiente);
        botones[siguiente].focus();
      });
    });

    mostrar(0);
  });

  /* ---------- copiar al portapapeles ---------- */

  function copiar(texto) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      return navigator.clipboard.writeText(texto);
    }
    return new Promise(function (resolver) {
      var area = document.createElement("textarea");
      area.value = texto;
      area.setAttribute("readonly", "");
      area.style.position = "fixed";
      area.style.opacity = "0";
      document.body.appendChild(area);
      area.select();
      try {
        document.execCommand("copy");
      } catch (e) {
        /* sin permiso: el texto sigue seleccionado para copiar a mano */
      }
      document.body.removeChild(area);
      resolver();
    });
  }

  document.querySelectorAll(".ventana, pre.copiable").forEach(function (bloque) {
    var pre = bloque.querySelector("pre");
    if (!pre) return;
    var boton = document.createElement("button");
    boton.className = "copiar";
    boton.type = "button";
    boton.textContent = "copiar";
    boton.addEventListener("click", function () {
      copiar(pre.innerText).then(function () {
        boton.textContent = "copiado";
        window.setTimeout(function () {
          boton.textContent = "copiar";
        }, 1400);
      });
    });
    if (pre.classList.contains("copiable")) pre.appendChild(boton);
    else bloque.querySelector("figcaption").appendChild(boton);
  });

  /* ---------- contadores ---------- */

  var contar = function (elemento) {
    var hasta = parseInt(elemento.dataset.cuenta, 10) || 0;
    var sufijo = elemento.dataset.sufijo || "";
    if (reducir || !hasta) {
      elemento.textContent = String(hasta) + sufijo;
      return;
    }
    var inicio = null;
    var duracion = 900;
    var paso = function (momento) {
      if (inicio === null) inicio = momento;
      var t = Math.min((momento - inicio) / duracion, 1);
      var valor = Math.round(hasta * (1 - Math.pow(1 - t, 3)));
      elemento.textContent = valor.toLocaleString("es-ES") + sufijo;
      if (t < 1) window.requestAnimationFrame(paso);
    };
    window.requestAnimationFrame(paso);
  };

  /* ---------- presupuesto de bytes ---------- */

  var relleno = document.getElementById("relleno");
  if (relleno) {
    var cuanto = 8896 / 12288;
    if (reducir) relleno.style.width = Math.round(cuanto * 100) + "%";
    else window.setTimeout(function () {
      relleno.style.width = Math.round(cuanto * 100) + "%";
    }, 250);
  }

  /* ---------- aparecer al bajar ---------- */

  var observables = [];
  var objetivos = document.querySelectorAll(".cifras b, .medidas b, .linea li, .tarjeta");
  Array.prototype.forEach.call(objetivos, function (el, i) {
    el.setAttribute("data-revelar", "");
    observables.push(el);
  });

  if (reducir || !("IntersectionObserver" in window)) {
    observables.forEach(function (el) {
      el.classList.add("visible");
    });
    Array.prototype.forEach.call(document.querySelectorAll("[data-cuenta]"), contar);
  } else {
    var observer = new IntersectionObserver(
      function (entradas) {
        entradas.forEach(function (entrada) {
          if (!entrada.isIntersecting) return;
          entrada.target.classList.add("visible");
          var cuenta = entrada.target.querySelector("[data-cuenta]");
          if (cuenta) {
            contar(cuenta);
            observer.unobserve(entrada.target);
          }
          if (!entrada.target.hasAttribute("data-cuenta")) observer.unobserve(entrada.target);
        });
      },
      { rootMargin: "0px 0px -12% 0px", threshold: 0.15 }
    );
    observables.forEach(function (el) {
      observer.observe(el);
    });
  }

  /* ---------- scroll-spy y progreso ---------- */

  var enlaces = document.querySelectorAll("#enlaces a");
  var secciones = [];
  Array.prototype.forEach.call(enlaces, function (a) {
    var id = a.getAttribute("href").slice(1);
    var seccion = document.getElementById(id);
    if (seccion) secciones.push({ a: a, seccion: seccion });
  });

  var progreso = document.getElementById("progreso");

  function alDesplazar() {
    var alto = document.body.scrollHeight - window.innerHeight;
    var pct = alto > 0 ? (window.scrollY / alto) * 100 : 0;
    progreso.style.width = pct.toFixed(2) + "%";
    progreso.setAttribute("aria-valuenow", Math.round(pct));

    var actual = null;
    secciones.forEach(function (par) {
      if (par.seccion.getBoundingClientRect().top <= window.innerHeight * 0.35) actual = par;
    });
    enlaces.forEach(function (a) {
      a.classList.remove("activo");
    });
    if (actual) actual.a.classList.add("activo");
  }

  var pendiente = false;
  window.addEventListener(
    "scroll",
    function () {
      if (pendiente) return;
      pendiente = true;
      window.requestAnimationFrame(function () {
        pendiente = false;
        alDesplazar();
      });
    },
    { passive: true }
  );
  window.addEventListener("resize", alDesplazar);
  alDesplazar();

  /* ---------- avisos si no hay JavaScript ---------- */

  var sinJs = document.getElementById("sin-js");
  if (sinJs) sinJs.hidden = false;
})();