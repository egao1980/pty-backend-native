(in-package #:pty-backend-native)

(defclass native-backend (pp:pty-backend) ()
  (:documentation "pty-protocol backend over libpty-cl."))

(defclass native-pty (pp:pty)
  ((handle :initarg :handle :reader %handle-cell
           :documentation "Cons cell holding the pty_cl_t pointer, shared with the
finalizer; NIL once closed.")
   (cols :initarg :cols :accessor %cols)
   (rows :initarg :rows :accessor %rows)))

(defun %handle (pty)
  (or (car (%handle-cell pty))
      (error 'pp:pty-closed-error :pty pty)))

(defun %ms (seconds)
  "Seconds (or NIL = forever) → the C ABI's milliseconds (-1 = forever)."
  (if seconds (max 0 (min most-positive-fixnum (round (* seconds 1000)))) -1))

(defun %string-array (strings)
  "A NULL-terminated char** of UTF-8 copies. Free with %FREE-STRING-ARRAY."
  (let ((array (cffi:foreign-alloc :pointer :count (1+ (length strings)))))
    (loop for s in strings for i from 0
          do (setf (cffi:mem-aref array :pointer i)
                   (cffi:foreign-string-alloc s :encoding :utf-8)))
    (setf (cffi:mem-aref array :pointer (length strings)) (cffi:null-pointer))
    array))

(defun %free-string-array (array)
  (loop for i from 0
        for p = (cffi:mem-aref array :pointer i)
        until (cffi:null-pointer-p p)
        do (cffi:foreign-string-free p))
  (cffi:foreign-free array))

(defparameter +signal-numbers+ '((:hup . 1) (:int . 2) (:kill . 9) (:term . 15))
  "The C ABI's portable signal numbers (same as Linux and macOS).")

(defmethod pp:backend-spawn-pty ((backend native-backend) command
                                 &key cols rows directory environment)
  (let ((argv (%string-array command))
        (envp (%string-array (loop for (k . v) in (pp:pty-environment-entries environment)
                                   collect (format nil "~A=~A" k v))))
        (cwd (if directory
                 (cffi:foreign-string-alloc directory :encoding :utf-8)
                 (cffi:null-pointer))))
    (unwind-protect
         (cffi:with-foreign-objects ((out :pointer) (err :int))
           (let ((result (%spawn (first command) argv envp cwd cols rows out err)))
             (unless (zerop result)
               (error 'pp:pty-spawn-error
                      :command command
                      :code (cffi:mem-ref err :int)
                      :message (%describe-error (cffi:mem-ref err :int))))
             (let* ((cell (list (cffi:mem-ref out :pointer)))
                    (pty (make-instance 'native-pty :backend backend :handle cell
                                                    :cols cols :rows rows)))
               (trivial-garbage:finalize pty (lambda ()
                                               (when (car cell)
                                                 (%close (car cell))
                                                 (setf (car cell) nil))))
               pty)))
      (%free-string-array argv)
      (%free-string-array envp)
      (unless (cffi:null-pointer-p cwd) (cffi:foreign-string-free cwd)))))

(defun %describe-error (code)
  #-windows
  (ignore-errors
   (cffi:foreign-string-to-lisp (cffi:foreign-funcall "strerror" :int code :pointer)))
  #+windows
  (case code (2 "file not found") (3 "path not found") (5 "access denied")
    (120 "ConPTY is not available (Windows 10 1809 or later is required)")
    (t nil)))

(defmethod pp:pty-read ((pty native-pty) &key timeout (max 4096))
  (check-type max (integer 1))
  (let ((buffer (make-array max :element-type '(unsigned-byte 8))))
    (let ((n (cffi:with-pointer-to-vector-data (p buffer)
               (%read (%handle pty) p max (%ms timeout)))))
      (cond ((plusp n) (subseq buffer 0 n))
            ((zerop n) (make-array 0 :element-type '(unsigned-byte 8)))
            ((= n -1) nil)
            (t (pp:signal-io-error pty :read (%errno (%handle pty)))
               (make-array 0 :element-type '(unsigned-byte 8)))))))

(defmethod pp:pty-write ((pty native-pty) octets &key (start 0) end)
  (let* ((end (or end (length octets)))
         (bytes (if (and (zerop start) (= end (length octets))
                         (typep octets '(simple-array (unsigned-byte 8) (*))))
                    octets
                    (coerce (subseq octets start end) '(simple-array (unsigned-byte 8) (*))))))
    (when (plusp (length bytes))
      (let ((n (cffi:with-pointer-to-vector-data (p bytes)
                 (%write (%handle pty) p (length bytes)))))
        (when (minusp n)
          (pp:signal-io-error pty :write (%errno (%handle pty)))))))
  nil)

(defmethod pp:pty-resize ((pty native-pty) cols rows)
  (when (minusp (%resize (%handle pty) cols rows))
    (pp:signal-io-error pty :resize (%errno (%handle pty))))
  (setf (%cols pty) cols (%rows pty) rows)
  nil)

(defmethod pp:pty-size ((pty native-pty))
  (values (%cols pty) (%rows pty)))

(defmethod pp:pty-signal ((pty native-pty) signal)
  (when (minusp (%signal (%handle pty) (cdr (assoc signal +signal-numbers+))))
    (pp:signal-io-error pty :signal (%errno (%handle pty))))
  nil)

(defun %signal-keyword (number)
  (or (car (rassoc number +signal-numbers+)) number))

(defmethod pp:pty-wait ((pty native-pty) &key timeout)
  (cffi:with-foreign-objects ((code :int) (sig :int))
    (let ((r (%wait (%handle pty) (%ms timeout) code sig)))
      (cond ((= r 1)
             (let ((c (cffi:mem-ref code :int)) (s (cffi:mem-ref sig :int)))
               (if (plusp s)
                   (values nil (%signal-keyword s))
                   (values c nil))))
            ((zerop r) nil)
            (t (pp:signal-io-error pty :wait (%errno (%handle pty))) nil)))))

(defmethod pp:pty-alive-p ((pty native-pty))
  ;; PTY-WAIT gives NIL NIL only on timeout; a signalled child is NIL :TERM.
  (multiple-value-bind (code signal) (pp:pty-wait pty :timeout 0)
    (and (null code) (null signal))))

(defmethod pp:pty-pid ((pty native-pty))
  (%pid (%handle pty)))

(defmethod pp:pty-close ((pty native-pty))
  (let ((cell (%handle-cell pty)))
    (when (car cell)
      (trivial-garbage:cancel-finalization pty)
      (%close (car cell))
      (setf (car cell) nil))))

(unless pp:*pty-backend*
  (setf pp:*pty-backend* (make-instance 'native-backend)))
