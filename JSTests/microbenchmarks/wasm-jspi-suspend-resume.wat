;; wat2wasm JSTests/microbenchmarks/wasm-jspi-suspend-resume.wat -o JSTests/microbenchmarks/wasm-jspi-suspend-resume.wasm
;; run(n) calls the imported Suspending function n times. Every call suspends
;; and resumes the whole Wasm stack. It returns the sum of the values the
;; import returned.
(module
  (import "env" "suspend" (func $suspend (param i32) (result i32)))

  (func (export "run") (param $n i32) (result i32)
    (local $i i32)
    (local $sum i32)
    (loop $continue
      (local.set $sum (i32.add (local.get $sum) (call $suspend (local.get $i))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $continue (i32.lt_u (local.get $i) (local.get $n))))
    (local.get $sum))
)
