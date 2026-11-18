(define-module (dragonfruit)
  #:use-module (ice-9 rdelim)
  #:use-module (ice-9 regex)
  #:use-module (guix download)
  #:use-module (guix git-download)
  #:use-module (guix gexp)
  #:use-module (guix packages)
  #:use-module (guix utils)
  #:use-module (gnu packages window-management)
  #:use-module (gnu packages gtk)
  #:use-module (gnu packages freedesktop)
  #:use-module (gnu packages xdisorg)
  #:use-module (gnu packages pciutils)
  #:use-module (gnu packages admin)
  #:use-module (gnu packages pcre)
  #:use-module (gnu packages javascript)
  #:use-module (gnu packages xorg)
  #:use-module (gnu packages build-tools)
  #:use-module (gnu packages ninja)
  #:use-module (gnu packages pkg-config)
  #:use-module (guix build-system meson)
  #:use-module ((guix licenses) #:prefix license:))



;; Builds the checkout this file lives in: the branch for the latest commit,
;; a release tag (git checkout 0.1.0) for that release.
;;   guix build -f packaging/guix/dragonfruit.scm
(define %root
  (canonicalize-path (string-append (current-source-directory) "/../..")))

(define %version
  (call-with-input-file (string-append %root "/meson.build")
    (lambda (port)
      (let loop ((line (read-line port)))
        (cond ((eof-object? line) "0")
              ((string-match "version : '([^']+)'" line)
               => (lambda (m) (match:substring m 1)))
              (else (loop (read-line port))))))))

(define-public dragonfruit
  (package
    (name "dragonfruit")
    (version %version)
    (source (local-file %root "dragonfruit-checkout"
                        #:recursive? #t
                        #:select? (or (git-predicate %root)
                                      (const #t))))
    (build-system meson-build-system)
    (arguments
     (list
      #:configure-flags
      #~(list (string-append "-Dsysconfdir=" #$output "/etc"))
      #:phases
      #~(modify-phases %standard-phases
          (add-before 'configure 'patch-meson
            (lambda _
              (substitute* "meson.build"
                ;; MangoWM ignores sysconfdir handling for NixOS.
                ;; We also need to skip that sysconfdir edits.
                (("is_nixos = false")
                 "is_nixos = true")
                ;; Unhardcode path.  Fixes loading default config.
                (("'-DSYSCONFDIR=\\\"@0@\\\"'.format\\('/etc'\\)")
                 "'-DSYSCONFDIR=\"@0@\"'.format(sysconfdir)")))))))
    (inputs (list wayland
                  libinput
                  libdrm
                  libxkbcommon
                  pixman
                  libdisplay-info
                  libliftoff
                  hwdata
                  seatd
                  pcre2
                  pango
                  cjson
                  libxcb
                  pixman
                  xcb-util-wm
                  wlroots-0.20
                  scenefx))
    (native-inputs (list pkg-config wayland-protocols))
    (home-page "https://codeberg.org/central-lambda-group/dragonfruit")
    (synopsis "Infinite-canvas Wayland compositor forked from Mango")
    (description
     "Dragonfruit is a Wayland compositor forked from Mango.  Windows live on
an infinite 2D canvas and the screen is a camera that pans and zooms over it.
A Mango configuration file works unchanged.")
    (license (list license:gpl3 ;dragonfruit, mango, dwl
                   license:expat ;dwm, sway, wlroots
                   license:cc0)))) ;tinywl

dragonfruit
