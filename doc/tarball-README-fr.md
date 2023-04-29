# Installation

1. Extraire la tarball si ce n'est pas déjà fait :

```shell
tar xvf texpresso.tar.gz
cd texpresso/
```

2. Remplacer `xetex` par la version patchée :

```shell
cp `which xetex`
```

3. On va lancer TeXpresso depuis Emacs, mais il faut que :

   - `texpresso` soit dans le `PATH` visible depuis emacs

   - `texpresso` et `libintexcept.so` soient dans le même dossier

   Deux possibilités :

   - copier directement texpresso et libintexcept.so dans un dossier dans le `PATH`, peut-être `~/.local/bin`:
     ```shell
     cp texpresso libintexcept.so ~/.local/bin
     ```

   - Les garder dans un dossier séparé et faire un lien symbolique, par exemple :

     ```shell
     ln -sf "$PWD/texpresso" ~/.local/bin
     ```

4. Charger le mode emacs :
   ```shell
   (load-file "<mydir>/texpresso.el")
   ```

# Utilisation

Depuis Emacs, il faut ouvrir un fichier .tex et activer `texpresso-mode` (c'est un mode mineur global).

À l'activation, il demande de choisir le fichier .tex racine. Pour un fichier auto-contenu, on peut juste valider pour utiliser le fichier courant. Pour un fichier multi-projet, on peut naviguer pour choisir la racine.

La fenêtre TeXpresso s'ouvre.
Contrôles claviers :

- ⬅️ gauche et ➡️ droite : changer de page
- `p` (pour "page") : changer le zoom entre "Pleine page" et "Pleine largeur"
- `c` (pour "crop") : enlever les bordures vides de la page
- `q` (pour "quit") : quitter
- `i` (pour "invert") : mode nuit
- `I` : utiliser le thème emacs (pour l'instant c'est juste le mien :D)
- `t` (pour "top") : garder la fenêtre au premier plan (au-dessus d'emacs donc)
- `b` (pour "border") : activer/désactiver les bordures de fenêtres
- `r` (pour "reload") : ne pas appuyer :D, c'est une fonctionnalité de "hot-reload" pour debug, mais ça risque de crasher

Contrôles souris :

- clique : tente de positionner le buffer emacs autour de la ligne sélectionnée
- contrôle + clique : faire défiler la page
- molette : faire défiler la page
- contrôle + molette : zoom