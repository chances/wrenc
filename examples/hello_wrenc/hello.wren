System.print("Hello, world!")

class Wren {
  flyTo(city) {
    System.print("Flying to %(city)")
  }

  static main() { // Called from C
    var adjectives = Fiber.new {
      ["small", "clean", "fast"].each {|word| Fiber.yield(word) }
    }

    while (!adjectives.isDone) System.print(adjectives.call())

    ForeignStuff.new(1234).doForeignThing()
  }
}

foreign class ForeignStuff {
  construct new(i) {}
  foreign doForeignThing()
}
