(define add_one (lambda (x) (+ x 1)))

;OUTPUT: 2
(println (add_one 1))

;OUTPUT: 43
(println (add_one 42))

;OUTPUT: 4
(println (apply add_one 3))