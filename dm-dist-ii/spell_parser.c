/* *********************************************************************** *
 * File   : spell_parser.c                            Part of Valhalla MUD *
 * Version: 1.05                                                           *
 * Author : seifert@diku.dk                                                *
 *                                                                         *
 * Purpose: Spell parser.                                                  *
 *                                                                         *
 * Bugs   : Unknown.                                                       *
 * Status : Unpublished.                                                   *
 *                                                                         *
 * Copyright (C) Valhalla (This work is unpublished).                      *
 *                                                                         *
 * This work is a property of:                                             *
 *                                                                         *
 *        Valhalla I/S                                                     *
 *        Noerre Soegade 37A, 4th floor                                    *
 *        1370 Copenhagen K.                                               *
 *        Denmark                                                          *
 *                                                                         *
 * This is an unpublished work containing Valhalla confidential and        *
 * proprietary information. Disclosure, use or reproduction without        *
 * authorization of Valhalla is prohobited.                                *
 * *********************************************************************** */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "structs.h"
#include "utils.h"
#include "skills.h"
#include "comm.h"
#include "db.h"
#include "interpreter.h"
#include "spells.h"
#include "handler.h"
#include "textutil.h"
#include "utility.h"
#include "dilrun.h"
#include "db_file.h"
#include "common.h"
#include "magic.h"
#include "justice.h"
#include "files.h"
#include "trie.h"

/* Extern data */
extern struct unit_data *unit_list;

struct spell_info_type spell_info[SPL_TREE_MAX];


void set_spellargs(struct spell_args *sa,
		   struct unit_data *caster,
		   struct unit_data *medium,
		   struct unit_data *target,
		   char *arg, int hm)
{
   sa->caster   = caster;
   sa->medium   = medium;
   sa->target   = target;
   sa->arg      = arg;
   sa->hm       = hm;
   sa->pEffect  = NULL;
}


/* Check if type is proper compared to specifications in the spell list */
/* above.                                                               */
ubit1 spell_legal_type(int spl, int type)
{
   return IS_SET(spell_info[spl].media, type);
}


/* Check if target is proper compared to specifications in the spell list */
/* above. Useable with for example wand checks to see if target is legal  */
ubit1 spell_legal_target(int spl, struct unit_data *caster,
			struct unit_data *target)
{
   int pk_test(struct unit_data *att, struct unit_data *def, int message);

   if (IS_SET(spell_info[spl].targets, TAR_IGNORE))
    return TRUE;

   if (caster == NULL ||
       target == NULL)
   {
      slog(LOG_ALL, 0, "NULL target in spell_legal_target");
      return FALSE;
   }

   if (spell_info[spl].offensive && pk_test(caster, target, TRUE))
     return FALSE;

  if (IS_SET(spell_info[spl].targets,
	     caster == target ? TAR_SELF_NONO : TAR_SELF_ONLY))
    return FALSE;

  if ((IS_SET(spell_info[spl].targets, TAR_ROOM) && IS_ROOM(target)) ||
      (IS_SET(spell_info[spl].targets, TAR_CHAR) && IS_CHAR(target)) ||
      (IS_SET(spell_info[spl].targets, TAR_OBJ)  && IS_OBJ(target)))
    return TRUE;

  return FALSE;
}



void say_spell(struct unit_data *ch, struct unit_data *target, int si)
{
   if (ch != target)
   {
      act(spell_info[si].tochar, A_ALWAYS, ch, 0, target, TO_CHAR);
      act(spell_info[si].tovict, spell_info[si].acttype,
	  ch, 0, target, TO_VICT);
      act(spell_info[si].torest, spell_info[si].acttype,
	  ch, 0, target, TO_NOTVICT);
   }
   else
   {
      act(spell_info[si].toself, A_ALWAYS, ch, 0, target, TO_CHAR);
      act(spell_info[si].toselfroom, spell_info[si].acttype,
	  ch, 0, target, TO_ROOM);
   }
}


int spell_perform(int spell_no, int spell_type,
		  struct unit_data *caster,
		  struct unit_data *medium,
		  struct unit_data *target,
		  char *argument, char *pEffect, int bonus)
{
   static struct command_info *cmd = NULL;
   int hm = -1;

   if (!cmd)
   {
      extern struct trie_type *intr_trie;

      cmd = (struct command_info *) search_trie("cast", intr_trie);
      assert(cmd);
   }

   if (!is_in(spell_no, SPL_NONE, SPL_TREE_MAX))
   {
      slog(LOG_ALL, 0, "Spell no %d is not in valid range.", spell_no);
      return -1;
   }

   if (spell_type == MEDIA_STAFF)
   {
      int i;
      int bitv = 0;

      if (IS_SET(spell_info[spell_no].targets, TAR_CHAR))
	SET_BIT(bitv, UNIT_ST_NPC | UNIT_ST_PC);

      if (IS_SET(spell_info[spell_no].targets, TAR_OBJ))
	SET_BIT(bitv, UNIT_ST_OBJ);

      scan4_unit(caster, bitv);

      for (i = 0; i < unit_vector.top; i++)
	hm = spell_perform(spell_no, MEDIA_WAND,
			   caster, medium, UVI(i), argument, pEffect);
      
      /* Self too... */
      if (!IS_SET(spell_info[spell_no].media, MEDIA_SELF_NONO))
	hm = spell_perform(spell_no, 
			   MEDIA_WAND,
			   caster, medium, caster, argument, pEffect);

      return hm;
   }

   if (!spell_legal_type(spell_no, spell_type))
   {
      send_to_char("This spell wasn't meant to "
		   "be there! Please report.\n\r", caster);

      szonelog(UNIT_FI_ZONE(medium),
	       "Illegal spell type (%d/%d): used on (%s@%s)!",
	       spell_no, spell_type,
	       UNIT_FI_NAME(medium), UNIT_FI_ZONENAME(medium));
      return -1;
   }

   if (!spell_legal_target(spell_no, caster, target))
   {
      act("The magic disappears when cast on $3n.",
	  A_SOMEONE, caster, NULL, target, TO_CHAR);
      return -1;
   }

   if (spell_info[spell_no].offensive && target && IS_CHAR(target))
     offend_legal_state(caster, target);

   switch (spell_info[spell_no].cast_type)
   {
     case SPLCST_CHECK:
      hm = spell_cast_check(medium, spell_no);
      break;

     case SPLCST_RESIST:
      hm = spell_resistance(medium, target, spell_no);
      break;

     case SPLCST_IGNORE:
      hm = 0;
      break;

     default:
      slog(LOG_ALL, 0, "Spell no %d had illegal cast type %d.",
	   spell_no, spell_info[spell_no].cast_type);
      return -1;
   }

   hm += bonus;

   char myarg[MAX_INPUT_LENGTH+10];

   strcpy(myarg, itoa(spell_no));
   strcat(myarg, argument);

   if (send_ack(caster, medium, target, &hm, cmd, myarg, target)==SFR_BLOCK)
     return hm;

   if (spell_info[spell_no].tmpl)
   {
      struct dilprg *prg;

      prg = dil_copy_template(spell_info[spell_no].tmpl, caster, NULL);
      prg->waitcmd = WAITCMD_MAXINST - 1; // The usual hack, see db_file

      prg->sp->vars[0].val.unitptr = medium;
      prg->sp->vars[1].val.unitptr = target;
      prg->sp->vars[2].val.string  = str_dup(argument);
      prg->sp->vars[3].val.integer = hm;
      prg->sp->vars[4].val.string  = str_dup(pEffect);

      dil_add_secure(prg, medium, prg->sp->tmpl->core);
      dil_add_secure(prg, target, prg->sp->tmpl->core);

      dil_activate(prg);
   }
   else if (spell_info[spell_no].spell_pointer)
   {
      struct spell_args sa;

      set_spellargs(&sa, caster, medium, target, argument, hm);
      sa.pEffect = pEffect;

      (*spell_info[spell_no].spell_pointer) (&sa);

      hm = sa.hm;
   }
   else
   {
      slog(LOG_ALL, 0, "Spell %d is not yet implemented.", spell_no);
      return -1;
   }

   send_done(caster, medium, target, spell_no, cmd, argument);

   return hm;
}


/* Assumes that argument does start with first letter of chopped string */
void do_cast(struct unit_data *ch, char *argument,
	     const struct command_info *cmd)
{
   struct unit_data *unit;
   int spl;
   ubit1 target_ok;
   char *orgarg, *c;

   if (str_is_empty(argument))
   {
      send_to_char("Cast which spell?\n\r", ch);
      return;
   }

   c = argument;
   spl = search_block_abbrevs(c, spl_text, (const char **) &argument);
   argument = skip_spaces(argument);

   if (spl == -1)
   {
      act("The $2t spell is not known to this realm.",
	  A_ALWAYS, ch, argument, 0, TO_CHAR);
      return;
   }

   if (spl < SPL_GROUP_MAX)
   {
      act("$2t is not a spell.", A_ALWAYS, ch, spl_text[spl], 0, TO_CHAR);
      return;
   }

   if (CHAR_POS(ch) < spell_info[spl].minimum_position)
   {
      switch(CHAR_POS(ch))
      {
	case POSITION_SLEEPING:
	 send_to_char("In your dreams?\n\r", ch);
	 break;
	case POSITION_RESTING:
	 send_to_char("You can't concentrate enough while resting.\n\r",ch);
	 break;
	case POSITION_SITTING:
	 send_to_char("You can't do this sitting!\n\r", ch);
	 break;
	case POSITION_FIGHTING:
	 send_to_char("Impossible! You can't concentrate enough!.\n\r", ch);
	 break;
	default:
	 send_to_char("It seems like you're in a pretty bad shape!\n\r",ch);
	 break;
      }
      /* Switch */
      return;
   }

   if (!IS_GOD(ch))
   {
      /* Check for page in spell book */

      /* Do this to prevent fighters from taking a chance that av will */
      /* make a natural success                                        */
      if (IS_PC(ch) && PC_SPL_SKILL(ch, spl) <= 1)
      {
	 send_to_char("Thou dost not knoweth of this magick.\n\r", ch);
	 return;
      }

      if (CHAR_ABILITY(ch, spell_info[spl].realm) == 0 ||
	  (CHAR_BRA(ch) == 0))
      {
	 send_to_char("You are not powerful enough.\n\r", ch);
	 return;
      }
   }
   
   if ((spell_info[spl].spell_pointer == NULL) && spell_info[spl].tmpl == NULL)
   {
      send_to_char("Sorry, this magic has not yet been implemented.\n\r", ch);
      return;
   }

   if (!spell_legal_type(spl, MEDIA_SPELL))
   {
      send_to_char("This isn't a legal spell for you to cast!  "
		   "Please report.\n\r", ch);
      slog(LOG_ALL, 0,
	   "do_cast: %s attempted to be used as spell.", spl_text[spl]);
      return;
   }

   /* **************** Locate targets **************** */

   target_ok = FALSE;
   unit = 0;

   orgarg = argument;

   if (!IS_SET(spell_info[spl].targets, TAR_IGNORE))
   {
      if (!str_is_empty(argument)) /* if there is an argument to look at */
      {
	 /* Find unit by the name 'argument' at target location */
	 unit = find_unit(ch, &argument, 0, spell_info[spl].targets);

	 if (unit)
	   target_ok = TRUE;
      }
      else			/* string is empty */
      {
	 if (IS_SET(spell_info[spl].targets, TAR_FIGHT_SELF) && CHAR_FIGHTING(ch))
	 {
	    unit = ch;
	    target_ok = TRUE;
	 }

	 if (!target_ok && IS_SET(spell_info[spl].targets, TAR_FIGHT_VICT)
	     && (unit = CHAR_FIGHTING(ch)))
	   target_ok = TRUE;

	 if (!target_ok && IS_SET(spell_info[spl].targets, TAR_AUTO_SELF))
	 {
	    unit = ch;
	    target_ok = TRUE;
	 }

	 if (!target_ok && IS_SET(spell_info[spl].targets, TAR_ROOM) &&
	     IS_ROOM(UNIT_IN(ch)))
	 {
	    unit = UNIT_IN(ch);
	    target_ok = TRUE;
	 }
      }

      if (!target_ok)
      {
	 if (*orgarg)		/* If a name was typed */
	 {
	    if (IS_SET(spell_info[spl].targets, TAR_CHAR))
	      act("Nobody by the name '$2t' here.",
		  A_ALWAYS, ch, orgarg, 0, TO_CHAR);
	    else if (IS_SET(spell_info[spl].targets, TAR_OBJ))
	      act("Nothing by the name '' here.",
		  A_ALWAYS, ch, orgarg, 0, TO_CHAR);
	    else if (IS_SET(spell_info[spl].targets, TAR_ROOM))
	      act("No location by the name '$2t'.",
		  A_ALWAYS, ch, orgarg, 0, TO_CHAR);
	    else
	      send_to_char("Uhm....??\n\r", ch);
	 }
	 else			/* Nothing was given as argument */
	   act("What should the $2t spell be cast upon?",
	       A_ALWAYS, ch, spl_text[spl],0, TO_CHAR);

	 return;
      }
      else
      {				/* TARGET IS OK */
	 if (unit == ch && IS_SET(spell_info[spl].targets, TAR_SELF_NONO))
	 {
	    send_to_char("You can not cast this spell upon yourself.\n\r", ch);
	    return;
	 }
	 else if (unit != ch && IS_SET(spell_info[spl].targets, TAR_SELF_ONLY))
	 {
	    send_to_char("You can only cast this spell upon yourself.\n\r", ch);
	    return;
	 }
      }
   }

#ifdef DEMIGOD
   if (IS_DEMIGOD(CHAR_ORIGINAL(ch)))
   {
      pow = spell_info[spl].demi_power * spell_info[spl].demi_power * 10000;

      if (unit && IS_CHAR(unit) && (ch != unit))
	pow = pow + (pow*CHAR_LEVEL(unit)) / 100;

      if (CHAR_EXP(CHAR_ORIGINAL(ch)) < pow)
      {
	 send_to_char("You don't have enough power points.\n\r", ch);
	 return;
      }

      act("You use $2d power points.", A_ALWAYS, ch, &pow, 0, TO_CHAR);

      CHAR_EXP(CHAR_ORIGINAL(ch)) -= pow;
   }
   else
#endif

     if (IS_MORTAL(ch))
     {
	if (CHAR_MANA(ch) < (sbit16) (spell_info[spl].usesmana))
	{
	   send_to_char("You can't summon enough energy to "
			"cast the spell.\n\r", ch);
	   return;
	}

	CHAR_MANA(ch) -= spell_info[spl].usesmana;
     }

   // Spells take time too!
   if (CHAR_COMBAT(ch))
     CHAR_COMBAT(ch)->changeSpeed(spell_info[spl].beats);

   say_spell(ch, unit, spl);

   spell_perform(spl, MEDIA_SPELL, ch, ch, unit, argument, NULL);
}


/* Boot time stuff below this line */


void spell_dil_check(void)
{
   char *dil_name;
   int i;

   for (i=0; i < SPL_TREE_MAX; i++)
   {
      if (spell_info[i].tmpl == NULL)
	continue;

      dil_name = (char *) spell_info[i].tmpl;
      spell_info[i].tmpl = find_dil_template(dil_name);
      
      if (spell_info[i].tmpl == NULL)
      {
	 slog(LOG_ALL, 0, "Spell template %s not found.", dil_name);
	 free(dil_name);
      }
      else
      {
	 if (spell_info[i].tmpl->argc != 5)
	 {
	    slog(LOG_ALL, 0, "Spell DIL %s argument count error, must be 5.",
		 dil_name);
	    free(dil_name);
	    spell_info[i].tmpl = NULL;
	    continue;
	 }

	 if (spell_info[i].tmpl->argt[0] != DILV_UP)
	 {
	    slog(LOG_ALL, 0, "Spell DIL %s argument 1 mismatch.",
		 dil_name);
	    free(dil_name);
	    spell_info[i].tmpl = NULL;
	    continue;
	 }

	 if (spell_info[i].tmpl->argt[1] != DILV_UP)
	 {
	    slog(LOG_ALL, 0, "Spell DIL %s argument 2 mismatch.",
		 dil_name);
	    free(dil_name);
	    spell_info[i].tmpl = NULL;
	    continue;
	 }

	 if (spell_info[i].tmpl->argt[2] != DILV_SP)
	 {
	    slog(LOG_ALL, 0, "Spell DIL %s argument 3 mismatch.",
		 dil_name);
	    free(dil_name);
	    spell_info[i].tmpl = NULL;
	    continue;
	 }

	 if (spell_info[i].tmpl->argt[3] != DILV_INT)
	 {
	    slog(LOG_ALL, 0, "Spell DIL %s argument 4 mismatch.",
		 dil_name);
	    free(dil_name);
	    spell_info[i].tmpl = NULL;
	    continue;
	 }

	 if (spell_info[i].tmpl->argt[4] != DILV_SP)
	 {
	    slog(LOG_ALL, 0, "Spell DIL %s argument 5 mismatch.",
		 dil_name);
	    free(dil_name);
	    spell_info[i].tmpl = NULL;
	    continue;
	 }
      }
   }
}


static void spell_read(void)
{
   int dummy, idx = -1;
   char pTmp[256];
   char *pCh;
   FILE *fl;

   touch_file(str_cc(libdir, SPELL_DEFS));
   if (!(fl=fopen(str_cc(libdir, SPELL_DEFS), "rb")))
   {
      slog(LOG_ALL, 0, "unable to create lib file");
      exit(0);
   }

   while (!feof(fl))
   {
      char *ms2020 = fgets(pTmp, sizeof(pTmp)-1, fl);
      str_remspc(pTmp);

      if ((pCh = strchr(pTmp, '=')))
      {
	 *pCh = 0;
	 pCh = skip_blanks(pCh + 1);
	 strip_trailing_blanks(pCh);
      }

      str_lower(pTmp);
      strip_trailing_blanks(pTmp);

      if (pCh == NULL || str_is_empty(pCh))
	continue;

      if (strncmp(pTmp, "index", 5) == 0)
      {
	 idx = atoi(pCh);
	 if (!str_is_number(pCh) || !is_in(idx, SPL_ALL, SPL_TREE_MAX-1))
	 {
	    slog(LOG_ALL, 0, "Spell boot error: %s", pCh);
	    idx = -1;
	 }
	 continue;
      }

      if (idx == -1)
	continue;

      if (strncmp(pTmp, "name", 4) == 0)
      {
	 if (spl_text[idx])
	   free(spl_text[idx]);
	 spl_text[idx] = str_dup(pCh);
      }
      else if (strncmp(pTmp, "tochar", 6) == 0)
      {
	 if (spell_info[idx].tochar)
	   free(spell_info[idx].tochar);
	 spell_info[idx].tochar = str_dup(pCh);
      }
      else if (strncmp(pTmp, "tovict", 6) == 0)
      {
	 if (spell_info[idx].tovict)
	   free(spell_info[idx].tovict);
	 spell_info[idx].tovict = str_dup(pCh);
      }
      else if (strncmp(pTmp, "torest", 6) == 0)
      {
	 if (spell_info[idx].torest)
	   free(spell_info[idx].torest);
	 spell_info[idx].torest = str_dup(pCh);
      }
      else if (strncmp(pTmp, "toselfroom", 10) == 0)
      {
	 if (spell_info[idx].toselfroom)
	   free(spell_info[idx].toselfroom);
	 spell_info[idx].toselfroom = str_dup(pCh);
      }
      else if (strncmp(pTmp, "toself", 6) == 0)
      {
	 if (spell_info[idx].toself)
	   free(spell_info[idx].toself);
	 spell_info[idx].toself = str_dup(pCh);
      }
      else if (strncmp(pTmp, "acttype", 7) == 0)
      {
	 dummy = atoi(pCh);
	 if (dummy == A_ALWAYS || dummy == A_HIDEINV ||
	     dummy == A_SOMEONE)
	   spell_info[idx].acttype = dummy;
      }
      else if (strncmp(pTmp, "sphere", 6) == 0)
      {
	 dummy = atoi(pCh);
	 if (is_in(dummy, SPL_NONE, SPL_GROUP_MAX-1))
	   spl_tree[idx].parent = dummy;
      }
      else if (strncmp(pTmp, "shield", 6) == 0)
      {
	 dummy = atoi(pCh);
	 if (is_in(dummy, SHIELD_M_BLOCK, SHIELD_M_USELESS))
	   spell_info[idx].shield = dummy;
      }
      else if (strncmp(pTmp, "realm", 5) == 0)
      {
	 dummy = atoi(pCh);
	 if (dummy == ABIL_MAG || dummy == ABIL_DIV)
	   spell_info[idx].realm = dummy;
      }
      else if (strncmp(pTmp, "func", 4) == 0)
      {
	 if (spell_info[idx].tmpl)
	   free(spell_info[idx].tmpl);
	   //MS2020 free(((char *) spell_info[idx].tmpl));
	 /* Nasty, just needed for brief conversion - MS2020 ... WTF is that ?!*/
	 spell_info[idx].tmpl = (struct diltemplate *) str_dup(pCh);
      }
      else if (strncmp(pTmp, "minpos", 6) == 0)
      {
	 dummy = atoi(pCh);
	 if (is_in(dummy, POSITION_DEAD, POSITION_STANDING))
	   spell_info[idx].minimum_position = dummy;
      }
      else if (strncmp(pTmp, "mana", 4) == 0)
      {
	 dummy = atoi(pCh);
	 if (is_in(dummy, 0, 100))
	   spell_info[idx].usesmana = dummy;
      }
      else if (strncmp(pTmp, "turns", 5) == 0)
      {
	 dummy = atoi(pCh);
	 if (is_in(dummy, 0, 4*PULSE_VIOLENCE))
	   spell_info[idx].beats = dummy;
      }
      else if (strncmp(pTmp, "targets", 7) == 0)
      {
	 dummy = atoi(pCh);
	 spell_info[idx].targets = dummy;
      }
      else if (strncmp(pTmp, "mediums", 7) == 0)
      {
	 dummy = atoi(pCh);
	 spell_info[idx].media = dummy;
      }
      else if (strncmp(pTmp, "check", 5) == 0)
      {
	 dummy = atoi(pCh);
	 if (dummy == SPLCST_IGNORE || dummy == SPLCST_RESIST ||
	     dummy == SPLCST_CHECK)
	   spell_info[idx].cast_type = dummy;
      }
      else if (strncmp(pTmp, "offensive", 9) == 0)
      {
	 dummy = atoi(pCh);
	 spell_info[idx].offensive = (dummy != 0);
      }
      else if (strncmp(pTmp, "race ", 5) == 0)
      {
	 dummy = atoi(pCh);
	 if (!is_in(dummy, -3, +3))
	   continue;

	 int ridx = search_block(pTmp+5, pc_races, TRUE);

	 if (ridx == -1)
	   slog(LOG_ALL, 0, "Spells: Illegal race in: %s", pTmp);
	 else
	   racial_spells[idx][ridx] = dummy;
      }
      else if (strncmp(pTmp, "fumble", 6) == 0)
      {
	 dummy = atoi(pCh);
	 if (is_in(dummy, 0, 99))
	   spell_chart[idx].fumble = dummy;
      }
      else if (strncmp(pTmp, "attack ", 7) == 0)
      {
	 char tmp[256];
	 int i1, i2, i3;
	 int idx2 = -1;

	 pCh = str_next_word(pCh, tmp);
	 i1 = atoi(tmp);
	 pCh = str_next_word(pCh, tmp);
	 i2 = atoi(tmp);
	 pCh = str_next_word(pCh, tmp);
	 i3 = atoi(tmp);

	 if (i3 <= 0)
	 {
	    slog(LOG_ALL, 0, "Spell init %d: Illegal damage series %d %d %d.",
		 idx, i1, i2, i3);
	    continue;
	 }

	 if (strncmp(pTmp+7, "clothes", 7) == 0)
	   idx2 = ARM_CLOTHES;
	 else if (strncmp(pTmp+7, "sleather", 8) == 0)
	   idx2 = ARM_LEATHER;
	 else if (strncmp(pTmp+7, "hleather", 8) == 0)
	   idx2 = ARM_HLEATHER;
	 else if (strncmp(pTmp+7, "chain", 5) == 0)
	   idx2 = ARM_CHAIN;
	 else if (strncmp(pTmp+7, "plate", 5) == 0)
	   idx2 = ARM_PLATE;

	 if (idx2 != -1)
	 {
	    spell_chart[idx].element[idx2].offset = i1;
	    spell_chart[idx].element[idx2].basedam = i2;
	    spell_chart[idx].element[idx2].alpha = i3;
	 }
      }
      else
	slog(LOG_ALL,0,"Spell boot unknown string: %s", pTmp);
   }

   fclose(fl);
}



static void spell_init(void)
{
   int i;

   for (i=0; i < SPL_TREE_MAX; i++)
   {
      spell_chart[i].fumble = 0;
      for (int j=0; j < 5; j++)
      {
	 spell_chart[i].element[j].offset  = 100;
	 spell_chart[i].element[j].basedam =   0;
	 spell_chart[i].element[j].alpha   = 100;
      }

      spell_info[i].tochar = NULL;
      spell_info[i].tovict = NULL;
      spell_info[i].torest = NULL;
      spell_info[i].toself = NULL;
      spell_info[i].toselfroom = NULL;
      spell_info[i].acttype = A_SOMEONE;

      spl_tree[i].parent = SPL_ALL;

      if (i < SPL_GROUP_MAX)
	spl_tree[i].isleaf = FALSE;
      else
	spl_tree[i].isleaf = TRUE;

      spell_info[i].spell_pointer = NULL;
      spell_info[i].minimum_position = POSITION_STANDING;
      spell_info[i].usesmana = 20;
      spell_info[i].beats = PULSE_VIOLENCE;
      spell_info[i].targets = 0;
      spell_info[i].media = 0;
      spell_info[i].cast_type = SPLCST_CHECK;
      spell_info[i].demi_power = 0;
      spell_info[i].offensive = FALSE;
      spell_info[i].tmpl = NULL;
      spell_info[i].shield = SHIELD_M_USELESS;
   
      spl_text[i] = NULL;

      /* Racial spells are all zero */
      for (int j=0; j < PC_RACE_MAX; j++)
	racial_spells[i][j] = 0;
   }

   spl_tree[SPL_TREE_MAX].parent = -1;
   spl_tree[SPL_TREE_MAX].isleaf = FALSE;
   spl_text[SPL_TREE_MAX] = NULL;
}


void boot_spell(void)
{
   spell_init();
   spell_read();

#define SPELLO(a,b) spell_info[a].spell_pointer = (b)

   SPELLO(SPL_REMOVE_CURSE, spell_remove_curse);
   SPELLO(SPL_CAUSE_WOUNDS_1, spell_cause_wounds_1);
   SPELLO(SPL_CAUSE_WOUNDS_2, spell_cause_wounds_2);
   SPELLO(SPL_CAUSE_WOUNDS_3, spell_cause_wounds_3);
   SPELLO(SPL_DISPEL_EVIL, spell_dispel_evil);
   SPELLO(SPL_REPEL_UNDEAD_1, spell_repel_undead_1);
   SPELLO(SPL_REPEL_UNDEAD_2, spell_repel_undead_2);
   SPELLO(SPL_CURE_BLIND, spell_cure_blind);
   SPELLO(SPL_SANCTUARY, spell_sanctuary);
   SPELLO(SPL_SUSTAIN, spell_sustain);
   SPELLO(SPL_LOCK, spell_lock);
   SPELLO(SPL_UNLOCK, spell_unlock);
   SPELLO(SPL_IDENTIFY_1, spell_identify_1);
   SPELLO(SPL_IDENTIFY_2, spell_identify_2);
   SPELLO(SPL_RANDOM_TELEPORT, spell_random_teleport);
   SPELLO(SPL_CLEAR_SKIES, spell_clear_skies);
   SPELLO(SPL_STORM_CALL, spell_storm_call);
   SPELLO(SPL_CONTROL_TELEPORT, spell_control_teleport);
   SPELLO(SPL_MINOR_GATE, spell_summon_char_1);
   SPELLO(SPL_GATE, spell_summon_char_2);
   SPELLO(SPL_ANIMATE_DEAD, spell_animate_dead);
   SPELLO(SPL_CONTROL_UNDEAD, spell_control_undead);
   SPELLO(SPL_COLOURSPRAY_1, spell_colourspray_1);
   SPELLO(SPL_COLOURSPRAY_2, spell_colourspray_2);
   SPELLO(SPL_COLOURSPRAY_3, spell_colourspray_3);
   SPELLO(SPL_INVISIBILITY, spell_invisibility);
   SPELLO(SPL_WIZARD_EYE, spell_wizard_eye);
   SPELLO(SPL_FEAR, spell_fear);
   SPELLO(SPL_CONFUSION, spell_confusion);
   SPELLO(SPL_XRAY_VISION, spell_xray_vision);
   SPELLO(SPL_COMMAND, spell_command);
   SPELLO(SPL_FIREBALL_1, spell_fireball_1);
   SPELLO(SPL_FIREBALL_2, spell_fireball_2);
   SPELLO(SPL_FIREBALL_3, spell_fireball_3);
   SPELLO(SPL_FROSTBALL_1, spell_frostball_1);
   SPELLO(SPL_FROSTBALL_2, spell_frostball_2);
   SPELLO(SPL_FROSTBALL_3, spell_frostball_3);
   SPELLO(SPL_LIGHTNING_1, spell_lightning_1);
   SPELLO(SPL_LIGHTNING_2, spell_lightning_2);
   SPELLO(SPL_LIGHTNING_3, spell_lightning_3);
   SPELLO(SPL_STINKING_CLOUD_1, spell_stinking_cloud_1);
   SPELLO(SPL_STINKING_CLOUD_2, spell_stinking_cloud_2);
   SPELLO(SPL_STINKING_CLOUD_3, spell_stinking_cloud_3);
   SPELLO(SPL_ACIDBALL_1, spell_acidball_1);
   SPELLO(SPL_ACIDBALL_2, spell_acidball_2);
   SPELLO(SPL_ACIDBALL_3, spell_acidball_3);
   SPELLO(SPL_MANA_BOOST, spell_mana_boost);
   SPELLO(SPL_DISPEL_GOOD, spell_dispel_good);
   SPELLO(SPL_TRANSPORT, spell_transport);
   SPELLO(SPL_UNDEAD_DOOR, spell_undead_door);
   
   spell_dil_check();
}
