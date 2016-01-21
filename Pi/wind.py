def rose(degrees):
    quadrants = ["N",
                 "NNE",
                 "NE",
                 "ENE",
                 "E",
                 "ESE",
                 "SE",
                 "SSE",
                 "S",
                 "SSW",
                 "SW",
                 "WSW",
                 "W",
                 "WNW",
                 "NW",
                 "NNW",
                 "N"]

    big_degrees = 371.25  + degrees
    quadrant = int(big_degrees/22.5)

    #force into range 0-15, with 0 = N 
    if quadrant >= 16:
        quadrant -= 16

    return quadrants[quadrant]
    
