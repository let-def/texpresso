;;; texpresso.el --- Render and synchronize buffers with TeXpresso -*- lexical-binding: t; -*-
;;
;; Copyright (C) 2023 Frédéric Bour
;; Hello world
;; Author: Frédéric Bour <frederic.bour@lakaban.net>
;; Maintainer: Frédéric Bour <frederic.bour@lakaban.net>
;; Created: March 25, 2023
;; Modified: March 25, 2023
;; Version: 0.0.1
;; Keywords: lisp local processes tex tools unix
;; Homepage: https://github.com/def/sync
;; Package-Requires: ((emacs "25.1"))
;;
;; This file is not part of GNU Emacs.
;;
;;; Commentary:
;;
;; TeXpresso is a tool to recompute LaTeX documents and error log interactively.
;;
;;; Code:

;; Customizable variables

(defcustom texpresso-binary nil
  "Path of TeXpresso binary."
  :group 'tex
  :risky t
  :type '(choice (file :tag "Path")
                 (const :tag "Auto" nil)))

(defcustom texpresso-follow-edition nil
  "If true, TeXpresso scrolls the view to the code being edited."
  :group 'tex
  :type 'boolean)

(defcustom texpresso-follow-cursor nil
  "If true, TeXpresso scrolls the view to the cursor."
  :group 'tex
  :type 'boolean)

;; Main code

(defvar texpresso--process nil
  "The running instance of TeXpresso as an Emacs process object, or nil.

`texpresso--process' is the latest process launched, it might be dead.

The process is also guaranteed to have a property named `'marker` that is
updated when synchronization state is reset.
When a buffer is synchronized with a process, it keeps a reference to the marker
object of that process. To check if incremental synchronization is possible, the
marker objects are compared for physical equality.
\(In practice they are `(cons nil nil)` objects, though their structural value is
not used anywhere.\)")

(defun texpresso--send (&rest value)
  "Send VALUE as a serialized s-expression to `texpresso--process'."
  (setq value (prin1-to-string value))
  ; (with-current-buffer (get-buffer-create "*texpresso-log*")
  ;   (let ((inhibit-read-only t))
  ;     (insert value)
  ;     (insert "\n")))
  (process-send-string texpresso--process value))

(defvar-local texpresso--state nil
  "Internal synchronization state for current buffer.
The state is either nil (buffer not yet synchronized) or a list
`(list (buffer-file-name) texpresso--process marker)'.
The list is used to detect if, since last synchronization, the filename has
changed, the process has changed, or the synchronization state was reset
\(new marker\).")

(defvar-local texpresso--before-change nil
  "A list (start end txt) saved during the last call to `texpresso--before-change'.
TeXpresso changes are expressed using byte offsets.
In the `after-change-functions' hook, one can only access the number of
characters removed by the change, and since the text has been already removed,
it is too late to access the number of bytes.  To work around this limitation,
the changed region is saved by the `texpresso--before-change' function (a
`before-change-functions' hook).")

(define-minor-mode texpresso-mode
  "A global minor mode that synchronizes buffer with TeXpresso.
Also launches a new TeXpresso process if none is running."
  :init-value nil   ; Initial value, nil for disabled
  :global nil
  :group 'tex
  :lighter " ☕"
  (if texpresso-mode
      (progn
        (message "TeXpresso ☕ enabled")
        (add-hook 'after-change-functions #'texpresso--after-change)
        (add-hook 'before-change-functions #'texpresso--before-change)
        (add-hook 'post-command-hook #'texpresso--post-command))
    (message "TeXpresso ☕ disabled")
    (remove-hook 'after-change-functions #'texpresso--after-change)
    (remove-hook 'before-change-functions #'texpresso--before-change)
    (remove-hook 'post-command-hook #'texpresso--post-command)))

(define-minor-mode texpresso-sync-mode
  "A minor mode that forces a buffer to be synchronized with TeXpresso.
Otherwise a buffer is synchronized if its major mode derives from `tex-mode'."
  :init-value nil   ; Initial value, nil for disabled
  :global nil
  :group 'tex
  :lighter " ☕"
  (setq texpresso--state nil))

(defvar-local texpresso--output-bound nil)
(defvar-local texpresso--output-timer nil)

(defun texpresso-move-to-cursor (&optional position)
  "Scroll TeXpresso views to POSITION (or point)."
  (interactive)
  (when (texpresso--enabled-p)
    (texpresso--send 'synctex-forward
                     (buffer-file-name)
                     (line-number-at-pos position t))))

(defun texpresso--output-truncate (buffer)
  "Truncate TeXpresso output buffer BUFFER."
  (with-current-buffer buffer
    (when texpresso--output-timer
      (cancel-timer texpresso--output-timer))
    (when (and texpresso--output-bound
               (<= texpresso--output-bound (point-max)))
      (let ((inhibit-read-only t))
        (delete-region texpresso--output-bound (point-max))))))

(defun texpresso--output-schedule-truncate (point)
  "Schedule a truncation of current buffer to POINT.
Scheduling allows truncation to not happen too often, slowing down the editor
and causing it to flicker."
  (when texpresso--output-timer
    (cancel-timer texpresso--output-timer))
  (setq texpresso--output-bound point)
  (setq texpresso--output-timer
        (run-with-timer 1 nil #'texpresso--output-truncate (current-buffer))))

(defun texpresso--enabled-p ()
  "Check if TeXpresso is running and enabled for the current buffer."
  (and (process-live-p texpresso--process)
       (or texpresso-sync-mode
           (derived-mode-p 'tex-mode))))

(defun texpresso--before-change (start end)
  "A `before-change-functions' hook to update `texpresso--before-change' variable.
It records the number of bytes between START and END (the bytes removed)."
  (when (texpresso--enabled-p)
    ; (message "before change %S %S" start end)
    (setq texpresso--before-change
          (list start end (buffer-substring-no-properties start end)))))

(defun texpresso--after-change (start end removed)
  "An `after-change-functions' hook to synchronize the buffer with TeXpresso.
It instructs `texpresso--process' to replace REMOVED characters by the contents
between START and END.
Character counts are converted to byte offsets using `texpresso--before-change'."
  (when (texpresso--enabled-p)
                                        ; (message "after change %S %S %S" start end removed)
    (let ((filename (nth 0 texpresso--state))
          (process  (nth 1 texpresso--state))
          (marker   (nth 2 texpresso--state))
          (bstart   (nth 0 texpresso--before-change))
          (bend     (nth 1 texpresso--before-change))
          (btext    (nth 2 texpresso--before-change))
          same-process)
      (setq same-process
            (and (eq filename (buffer-file-name))
                 (eq process  texpresso--process)
                 (eq marker   (process-get texpresso--process 'marker))))
      (if (and same-process (<= bstart start (+ start removed) bend))
          (let ((ofs (- start bstart)))
            (texpresso--send 'change filename (1- (position-bytes start))
                             (string-bytes (substring btext ofs (+ ofs removed)))
                             (buffer-substring-no-properties start end)))
        (when same-process
          (message "TeXpresso: change hooks called with invalid arguments")
          (message "(before-change %S %S %S)" bstart bend btext)
          (message "(after-change %S %S %S)" start end removed))
        (when (process-live-p process)
          (process-send-string
           process (prin1-to-string (list 'close filename))))
        (setq texpresso--state
              (list (buffer-file-name) texpresso--process
                    (process-get texpresso--process 'marker)))
        (texpresso--send 'open (buffer-file-name)
                         (buffer-substring-no-properties
                          (point-min) (point-max))))
      (when texpresso-follow-edition
        (texpresso--send 'synctex-forward
                         (buffer-file-name)
                         (line-number-at-pos nil t))))))

(defun texpresso--post-command ()
  "Function executed on post-command hook.
Sends cursor position to TeXpresso if `texpresso-follow-cursor'."
  (when texpresso-follow-cursor
    (texpresso-move-to-cursor)))

(defun texpresso--stderr-filter (process text)
  "Save debug TEXT from TeXpresso PROCESS in *texpresso-stderr* buffer.
The output is truncated to ~50k."
  (let ((buffer (process-buffer process)))
    (when buffer
      (with-current-buffer buffer
        (save-excursion
          (when (> (point-max) 49152)
            (delete-region (point-min) 16384))
          (goto-char (point-max))
          (insert text))))))

(defun texpresso--display-output (buffer)
  "Display BUFFER in a small window at bottom."
  (if nil ;(fboundp '+popup/buffer)
      (with-current-buffer buffer (+popup/buffer))
    (display-buffer-at-bottom buffer '(nil (allow-no-window . t) (window-height . 0.2)))))

(defun texpresso--get-output-buffer (name &optional force)
  "Return the buffer associated to TeXpresso channel NAME.
TeXpresso forwards different outputs of TeX process.
The standard output is named `'out' and the log file `'log'.
If it doesn't exists and FORCE is set, the buffer is created, otherwise nil is
returned."
  (let (fullname buffer)
    (setq fullname (cond
                    ((eq name 'out) "*texpresso-out*")
                    ((eq name 'log) "*texpresso-log*")
                    (t (error "TeXpresso: unknown buffer %S" name))))
    (setq buffer (get-buffer fullname))
    (when (and (not buffer) force)
      (setq buffer (get-buffer-create fullname))
      (with-current-buffer buffer
        (setq buffer-read-only t
              buffer-undo-list t)
        (when (eq name 'out)
          (compilation-mode)
          (texpresso--display-output buffer))))
    buffer))

(defun texpresso-display-output ()
  "Open a small window to display TeXpresso output messages."
  (interactive)
  (texpresso--display-output (texpresso--get-output-buffer 'out 'force)))

(defun texpresso--stdout-dispatch (process expr)
  "Interpret s-expression EXPR sent by TeXpresso PROCESS.
TeXpresso communicates with Emacs by writing a sequence of s-expressions on its
standard output. This function interprets one of these."
  (let ((tag (car expr)))
    (cond
     ((eq tag 'reset-sync)
      (process-put process 'marker (cons nil nil)))

     ((eq tag 'truncate)
      (let ((buffer (texpresso--get-output-buffer (nth 1 expr))))
        (when buffer
          (with-current-buffer buffer
            (let ((pos (byte-to-position (1+ (nth 2 expr)))))
              (when pos
                (texpresso--output-schedule-truncate pos)))))))


     ((eq tag 'append)
      (with-current-buffer (texpresso--get-output-buffer (nth 1 expr) 'force)
        (let ((inhibit-read-only t)
              (pos (byte-to-position (1+ (nth 2 expr))))
              (text (nth 3 expr))
              (window (get-buffer-window))
              lines endpos)
          (setq endpos (+ pos (length text)))
          (unless (and (>= (point-max) endpos)
                       (string= text (buffer-substring pos endpos)))
            (goto-char pos)
            (setq lines (line-number-at-pos pos))
            (insert text)
            (setq lines (- (line-number-at-pos (point)) lines))
            (when (> lines 0)
              (save-excursion
                (let ((beg (point)))
                  (forward-line lines)
                  (delete-region beg (point)))))
            (when window (with-selected-window window
                           (goto-char (1- (point-max)))
                           (recenter -1))))
          (texpresso--output-schedule-truncate endpos))))

     ((eq tag 'flush)
      (dolist (buffer (list (texpresso--get-output-buffer 'out)
                            (texpresso--get-output-buffer 'log)))
        (when buffer (texpresso--output-truncate buffer))))

     ((eq tag 'synctex)
      (let ((fname (nth 1 expr)) buf)
        (setq buf (and (file-exists-p fname)
                       (if (string= (buffer-name) "*TeXpresso window*")
                           (find-file-other-window fname)
                         (find-file fname))))
        (if buf
            (with-current-buffer buf
              (goto-char (point-min))
              (forward-line (1- (nth 2 expr)))
              (switch-to-buffer buf))
          (message "TeXpresso: unknown file %s" (nth 1 expr)))))

     (t (message "Unknown message in texpresso output: %S" expr)))))

(defun texpresso--stdout-filter (process text)
  "Interpret output of TeXpresso PROCESS.
TeXpresso communicates with Emacs by writing a sequence of textual s-expressions
on its standard output. This function receives a chunk of this TEXT, parses and
forwards the complete ones to `texpresso--stdout-dispatch', and buffers the
remainder."
  (let ((prefix (process-get process 'buffer)))
    (when prefix (setq text (concat prefix text))))
  (let ((pos 0))
    (condition-case nil
        (while t
          (let ((result (read-from-string text pos)))
            (setq pos (cdr result))
            (condition-case-unless-debug err
                (texpresso--stdout-dispatch process (car result))
              (error (message
                      "Error in texpresso--stdout-dispatch: %S\nWhile processing: %S"
                      err (car result))))))
      ((end-of-file)
       (process-put process 'buffer (substring text pos))))))

(defun texpresso-reset ()
  "Invalidate the synchronization state of all buffers."
  (interactive)
  (when texpresso--process
    (process-put texpresso--process 'marker (cons nil nil))))

(defun texpresso-reset-buffer ()
  "Invalidate the synchronization state of current buffer."
  (interactive)
  (setq texpresso--state nil))

(defadvice enable-theme (after texpresso--theme-change protect activate)
  "Tell TeXpresso about new theme colors."
  (when (process-live-p texpresso--process)
    (texpresso--send 'theme
                     (color-name-to-rgb (face-attribute 'default :background))
                     (color-name-to-rgb (face-attribute 'default :foreground)))))

(defun texpresso--make-process (&rest command)
  "Create and setup a new TeXpresso process with given COMMAND."
  (when (process-live-p texpresso--process)
    (kill-process texpresso--process))
  (let ((texpresso-stderr (get-buffer-create "*texpresso-stderr*")))
    (with-current-buffer texpresso-stderr (setq buffer-undo-list t))
    (dolist (buffer (list (texpresso--get-output-buffer 'out 'force)
                          (texpresso--get-output-buffer 'log)))
      (let ((inhibit-read-only t))
        (when buffer
          (with-current-buffer buffer
            (delete-region (point-min) (point-max))))))
    (setq texpresso--process
          (make-process :name "texpresso"
                        :stderr texpresso-stderr
                        :connection-type 'pipe
                        :command command))
    (set-process-filter (get-buffer-process texpresso-stderr)
                        #'texpresso--stderr-filter)
    (set-process-filter texpresso--process
                        #'texpresso--stdout-filter)
    (process-put texpresso--process 'marker (cons nil nil))
    (texpresso--send 'theme
                     (color-name-to-rgb (face-attribute 'default :background))
                     (color-name-to-rgb (face-attribute 'default :foreground)))))

(defun texpresso-connect-debugger ()
  "Create a new TeXpresso process using the debug proxy.
Normal TeXpresso processes are started using `texpresso-mode' or
`texpresso-restart'.  This function is reserved for debugging purposes. It
connects to an existing TeXpresso instance launched in a terminal using
\"texpresso-debug\" shell command.
I came up with this workflow because Emacs (29.0.60 on macOS) bugged
when attaching a debugger to a process it launched. More specifically,
the bug was that the first interaction was successful, but then Emacs marked the
process as exited (in `process-status') and no more contents could be sent to
it, even though the process was still sending its stderr to Emacs."
  (interactive)
  (texpresso--make-process "texpresso-debug-proxy"))

(defun texpresso (&optional filename)
  "Start a new TeXpresso process using FILENAME as the master TeX file.
When called interactively with a prefix argument, ask for the file.
If FILENAME is nil, use `TeX-master' from AUCTeX or `buffer-file-name'."
  (interactive "P")
  (unless texpresso-mode
    (texpresso-mode 1))

  (let ((tm-fn (when (boundp 'TeX-master)
                 (TeX-master-file t))))
    (if (or (consp filename) (numberp filename)
            (and (called-interactively-p) (null filename) (null tm-fn)))
        ;; called interactively with a prefix or default unavailable
        (setq filename (read-file-name "TeX root file: " nil tm-fn))
      ;; called interactively without prefix or from lisp, fall back
      (unless filename (setq filename tm-fn)))

    (unless filename (error "TeXpresso: no valid TeX root file available.")))

  (condition-case err
      (texpresso--make-process (or texpresso-binary "texpresso")
                               (expand-file-name filename))
    ((file-missing)
     (customize-variable 'texpresso-binary)
     (message "Cannot launch TeXpresso. Please select the executable file and try again. (error: %S)"
              (cdr err)))))

(defun texpresso-signal ()
  "Tell TeXpresso processes to check filesystem for changed files.
This is an alternative, more manual, workflow.
During development, it can also be used to hot-reload TeXpresso code."
  (interactive)
  (call-process "killall" nil 0 nil "-SIGUSR1" "texpresso"))

(defun texpresso-previous-page ()
  "Tell TeXpresso to move to previous page."
  (interactive)
  (texpresso--send 'previous-page))

(defun texpresso-next-page ()
  "Tell TeXpresso to move to next page."
  (interactive)
  (texpresso--send 'next-page))

(provide 'texpresso)
;;; texpresso.el ends here
