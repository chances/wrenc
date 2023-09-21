// ========
// Comments
// ========

// Single-line comments.
// ^ comment

/* Block comments. */
// ^ comment

/*
    Block comments.
*/
// <- comment

// ============
// Declarations
// ============

var true_ = true
// <- keyword
//   ^ variable
//            ^ keyword.bool
var false_ = false
// <- keyword
//   ^ variable
//             ^ keyword.bool
var num = 8
// <- keyword
//   ^ variable
//        ^ number
var str = "number"
// <- keyword
//   ^ variable
//        ^ string
var null_ = null
// <- keyword
//   ^ variable
//           ^ keyword.null

// =========
// Functions
// =========
var fn = Fn.new {}
// <- keyword
//   ^ variable
//              ^ function.closure

// =================
// Classes & Methods
// =================

class Clazz {
    // <- keyword
    //  ^ class
    // FIXME: This should also highlight with the `class` tag
    my_method(arg, a, b, c) {
        // <- method
        //     ^ variable.parameter
        //         ^ variable.parameter
        //            ^ variable.parameter
        //               ^ variable.parameter
        if (arg == 123) {
            //      ^ number
            return null
            // <- keyword
            //      ^ keyword.null
        }
        return arg + 1
        // ^ keyword
        //           ^ number
    }
    
    zeroGetter { 0 }
    // ^ method
    //           ^ number
    nullGetter { null }
    // ^ method
    //            ^ keyword.null

    setter=(value) {
        // <- method
        //    ^ variable.parameter
        this.abc = 12.45
        // <- keyword
        //           ^ number
        System.print(1234, value.hello)
        System.print(1, 2, 3, 4, 5)
    }

    foreign extern_method(a, b, c)
    // <- keyword
    //            ^ method
    //                    ^ variable.parameter
    //                       ^ variable.parameter
    //                          ^ variable.parameter
}
