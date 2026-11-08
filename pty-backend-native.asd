(defsystem "pty-backend-native"
  :version "0.2.1"
  :description "pty-protocol backend: forkpty-style spawn on Unix, ConPTY on Windows (small C library)"
  :author "egao1980"
  :license "MIT"
  :defsystem-depends-on ("asdf-native")
  :depends-on ("pty-protocol" "cffi" "uiop" "trivial-garbage")
  :serial t
  :pathname "src"
  :components ((:file "package")
               ;; $PTY_CL_NATIVE, native/ (overlay), lib/<os>-<arch>/ (checkout)
               (:native-library "pty-cl")
               (:file "ffi")
               (:file "backend"))
  :in-order-to ((test-op (test-op "pty-backend-native/tests")))
  ;; Overlays: lib/<os>-<arch>/ per platform, from the :native-library components.
  :properties (:cl-repo (:provides ("pty-backend-native"))))

(defsystem "pty-backend-native/tests"
  :depends-on ("pty-backend-native" "rove")
  :pathname "tests"
  :serial t
  :components ((:file "package")
               (:file "backend-test"))
  :perform (test-op (o c)
             (unless (symbol-call :rove :run c)
               (error "tests failed for ~A" (component-name c)))))
