(in-package #:pty-backend-native/tests)

;;; Real children on a real pty. Unix tests use /bin/sh; Windows uses cmd.exe,
;;; whose ConPTY output also carries VT setup sequences, so Windows checks
;;; search for text rather than compare bytes. On Windows Server 2022, ConPTY
;;; drops the last frame of a client that exits immediately after writing,
;;; so the Windows commands linger a moment (ping) before exiting.

(defun octets (s) (map '(vector (unsigned-byte 8)) #'char-code s))

(defun text (octets)
  (map 'string #'code-char octets))

(defun read-all (pty &key (timeout 5))
  "Read until EOF (or TIMEOUT seconds) → a string."
  (let ((deadline (+ (get-internal-real-time) (* timeout internal-time-units-per-second)))
        (out (make-string-output-stream)))
    (loop for chunk = (pp:pty-read pty :timeout 0.2)
          while chunk
          do (write-string (text chunk) out)
          until (> (get-internal-real-time) deadline))
    (get-output-stream-string out)))

(defun read-until (pty needle &key (timeout 5))
  "Read until NEEDLE shows up → everything read, or NIL on timeout/EOF."
  (let ((deadline (+ (get-internal-real-time) (* timeout internal-time-units-per-second)))
        (seen ""))
    (loop for chunk = (pp:pty-read pty :timeout 0.2)
          while chunk
          do (setf seen (concatenate 'string seen (text chunk)))
             (when (search needle seen) (return seen))
          until (> (get-internal-real-time) deadline))))

(deftest backend-installed
  (ok (typep pp:*pty-backend* 'pn:native-backend)))

#-windows
(progn
  (deftest output-and-exit-code
    (pp:with-pty (p '("sh" "-c" "printf hello; exit 7"))
      (ok (search "hello" (read-all p)))
      (ok (equal (multiple-value-list (pp:pty-wait p :timeout 5)) '(7 nil)))
      (ng (pp:pty-alive-p p))))

  (deftest child-sees-a-tty
    (pp:with-pty (p '("sh" "-c" "test -t 0 && test -t 1 && printf tty"))
      (ok (search "tty" (read-all p)))))

  (deftest initial-size
    (pp:with-pty (p '("sh" "-c" "stty size") :cols 100 :rows 30)
      (ok (search "30 100" (read-all p)))))

  (deftest resize-reaches-child
    (pp:with-pty (p '("sh" "-c" "read x; stty size") :cols 80 :rows 24)
      (pp:pty-resize p 50 10)
      (ok (equal (multiple-value-list (pp:pty-size p)) '(50 10)))
      (pp:pty-write p (octets (format nil "go~C" #\Return)))
      (ok (read-until p "10 50"))))

  (deftest echo-input
    (pp:with-pty (p '("cat"))
      (pp:pty-write p (octets (format nil "ping~C" #\Return)))
      (ok (read-until p "ping"))
      (pp:pty-write p (octets (string (code-char 4))))   ; ^D
      (ok (equal (multiple-value-list (pp:pty-wait p :timeout 5)) '(0 nil)))))

  (deftest environment-and-term
    (pp:with-pty (p '("sh" "-c" "printf '%s|%s' \"$FOO\" \"$TERM\"")
                    :environment '(("FOO" . "bar")))
      (ok (search "bar|xterm-256color" (read-all p))))
    (pp:with-pty (p '("/usr/bin/env") :inherit-environment nil
                                      :environment '(("ONLY" . "1")))
      (let ((out (read-all p)))
        (ok (search "ONLY=1" out))
        (ng (search "HOME=" out)))))

  (deftest working-directory
    (pp:with-pty (p '("pwd") :directory "/")
      (ok (search "/" (read-all p)))))

  (deftest signals-and-timeouts
    (pp:with-pty (p '("sleep" "30"))
      (ok (pp:pty-alive-p p))
      (ng (pp:pty-wait p :timeout 0.1))
      (ok (zerop (length (pp:pty-read p :timeout 0.1))))
      (pp:pty-signal p :term)
      (ok (equal (multiple-value-list (pp:pty-wait p :timeout 5)) '(nil :term)))
      (ng (pp:pty-alive-p p))))

  (deftest spawn-error-keeps-errno
    (let ((c (handler-case (pp:spawn-pty '("definitely-not-a-command-xyz"))
               (pp:pty-spawn-error (c) c))))
      (ok (typep c 'pp:pty-spawn-error))
      (ok (eql 2 (pp:pty-spawn-error-code c)))   ; ENOENT
      (ok (stringp (pp:pty-spawn-error-message c)))))

  (deftest close-kills-a-running-child
    (let ((p (pp:spawn-pty '("sleep" "30"))))
      (pp:pty-close p)
      (ok (pp:pty-closed-p p))
      (ok (signals (pp:pty-read p) 'pp:pty-closed-error))))

  (deftest many-ptys-and-gc
    (dotimes (i 40)
      (let ((p (pp:spawn-pty '("sh" "-c" "printf x"))))
        (if (evenp i) (pp:pty-close p) (read-all p :timeout 1))))
    (trivial-garbage:gc :full t)
    (pp:with-pty (p '("sh" "-c" "printf ok"))
      (ok (search "ok" (read-all p))))))

#+windows
(progn
  (deftest output-and-exit-code
    (pp:with-pty (p '("cmd.exe" "/c" "echo hello& ping -n 2 127.0.0.1 >nul& exit 7"))
      (let ((out (read-all p)))
        (ok (search "hello" out) (format nil "read ~S" out)))
      (ok (equal (multiple-value-list (pp:pty-wait p :timeout 10)) '(7 nil)))))

  (deftest initial-size
    (pp:with-pty (p '("cmd.exe" "/c" "mode con") :cols 100 :rows 30)
      (ok (search "100" (read-all p :timeout 10)))))

  (deftest environment
    (pp:with-pty (p '("cmd.exe" "/c" "echo %FOO%& ping -n 2 127.0.0.1 >nul")
                    :environment '(("FOO" . "bar")))
      (let ((out (read-all p :timeout 10)))
        (ok (search "bar" out) (format nil "read ~S" out)))))

  (deftest spawn-error-keeps-code
    (let ((c (handler-case (pp:spawn-pty '("definitely-not-a-command-xyz.exe"))
               (pp:pty-spawn-error (c) c))))
      (ok (typep c 'pp:pty-spawn-error))
      (ok (eql 2 (pp:pty-spawn-error-code c)))))

  (deftest terminate-and-close
    (let ((p (pp:spawn-pty '("cmd.exe" "/c" "ping -n 30 127.0.0.1"))))
      (ok (pp:pty-alive-p p))
      (pp:pty-signal p :kill)
      (ok (pp:pty-wait p :timeout 10))
      (pp:pty-close p)
      (ok (pp:pty-closed-p p)))))
